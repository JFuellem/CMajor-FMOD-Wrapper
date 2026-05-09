#pragma once

#include "fmod.h"
#include "fmod_dsp.h"
#include "json.hpp"
#include "dr_wav.h"
#include "dr_mp3.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include <regex>

#ifndef CMAJOR_CODEGEN_MAX_FRAMES_PER_BLOCK
#define CMAJOR_CODEGEN_MAX_FRAMES_PER_BLOCK 8192
#endif

using json = nlohmann::json;

template <typename Processor>
class CmajorWrapper
{
public:
    enum class BufferLoadResult {
        Queued,
        NotDataParameter,
        DecodeFailed,
        UnsupportedEndpointType
    };

    struct BufferEndpointInfo {
        std::string id;
        std::string name;
        uint32_t handle = 0;
    };

    CmajorWrapper()
    {
    }

    ~CmajorWrapper()
    {
        if (deInterleaveBuffer) delete[] deInterleaveBuffer;
        if (interleaveBuffer) delete[] interleaveBuffer;
    }

    // Metadata-only (GetDSPDescription)
    void Init(double sampleRate)
    {
        this->sampleRate = sampleRate;
        InitMetadata();
    }

    void InitWithHost(double sampleRate, int hostBlockFrames)
    {
        this->sampleRate = sampleRate;
        lastHostBlockFrames = hostBlockFrames;

        InitMetadata();

        int32_t c = 0;
        do {
            processors.push_back(std::make_unique<Processor>());
            processors.back()->initialise(c, sampleRate);
            ++c;
        } while (multiChannelExpandable && c < 32);

        ApplyCurrentParametersToProcessors();
    }

    void InitMetadata()
    {
        parameters.clear();
        bufferEndpoints.clear();
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            pendingBuffers.clear();
            activeBuffers.clear();
        }

        // Parse JSON for parameter and bus info without instantiating the generated processor.
        const json j = json::parse(Processor::programDetailsJSON);
        
        if (j.contains("inputs")) {
            for (const auto& input : j["inputs"]) {
                const bool isBufferEndpoint = IsBufferLoadableInputEndpoint(input);

                if (!isBufferEndpoint && input["endpointType"] == "event" && input["dataType"]["type"] == "float32") {
                    ParamInfo info;
                    info.id = input["endpointID"].template get<std::string>();
                    info.name = info.id;
                    info.min = 0.0f;
                    info.max = 1.0f;
                    info.init = 0.0f;
                    info.unit = "";
                    
                    if (input.contains("annotation")) {
                        auto& ann = input["annotation"];
                        if (ann.contains("name")) info.name = ann["name"].template get<std::string>();
                        if (ann.contains("min")) info.min = ann["min"].template get<float>();
                        if (ann.contains("max")) info.max = ann["max"].template get<float>();
                        if (ann.contains("init")) info.init = ann["init"].template get<float>();
                        if (ann.contains("unit")) info.unit = ann["unit"].template get<std::string>();
                    }
                    
                    // Find the endpoint handle
                    for (const auto& ep : Processor::inputEndpoints) {
                        if (std::string(ep.name) == info.id) {
                            info.handle = ep.handle;
                            break;
                        }
                    }
                    
                    info.currentValue = info.init;
                    parameters.push_back(info);
                }

                if (isBufferEndpoint) {
                    BufferEndpointInfo info;
                    info.id = input["endpointID"].template get<std::string>();
                    info.name = info.id;

                    if (input.contains("annotation")) {
                        const auto& ann = input["annotation"];
                        if (ann.contains("name")) {
                            info.name = ann["name"].template get<std::string>();
                        }
                    }

                    bool foundHandle = false;
                    for (const auto& ep : Processor::inputEndpoints) {
                        if (std::string(ep.name) == info.id) {
                            info.handle = ep.handle;
                            foundHandle = true;
                            break;
                        }
                    }

                    if (foundHandle)
                        bufferEndpoints.push_back(std::move(info));
                }
            }
        }
        
        // Count audio I/O
        numInputChannels = 0;
        numOutputChannels = 0;
        
        if (j.contains("inputs")) {
            for (const auto& input : j["inputs"]) {
                if (input["endpointType"] == "stream" && input.contains("numAudioChannels")) {
                    numInputChannels += input["numAudioChannels"].template get<int>();
                    
                    for (const auto& ep : Processor::inputEndpoints) {
                        if (std::string(ep.name) == input["endpointID"].template get<std::string>()) {
                            audioInputHandle = ep.handle;
                            break;
                        }
                    }
                }
            }
        }
        
        if (j.contains("outputs")) {
            for (const auto& output : j["outputs"]) {
                if (output["endpointType"] == "stream" && output.contains("numAudioChannels")) {
                    numOutputChannels += output["numAudioChannels"].template get<int>();
                    
                    for (const auto& ep : Processor::outputEndpoints) {
                        if (std::string(ep.name) == output["endpointID"].template get<std::string>()) {
                            audioOutputHandle = ep.handle;
                            break;
                        }
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            pendingBuffers.resize(bufferEndpoints.size());
            activeBuffers.resize(bufferEndpoints.size());
        }

        multiChannelExpandable = (numInputChannels == 1 && numOutputChannels == 1);
    }

    // Re-read host sample rate / nominal block from FMOD; re-initialise processors if the rate changed.
    void SyncFromFmod(FMOD_DSP_STATE* dsp_state)
    {
        if (!dsp_state || processors.empty())
            return;

        int rate = 44100;
        unsigned int block = 512;
        FMOD_DSP_GETSAMPLERATE(dsp_state, &rate);
        FMOD_DSP_GETBLOCKSIZE(dsp_state, &block);

        if (block != lastHostBlockFrames)
            lastHostBlockFrames = block;

        const double newRate = static_cast<double>(rate);
        if (newRate != sampleRate)
        {
            sampleRate = newRate;
            for (size_t i = 0; i < processors.size(); ++i)
                processors[i]->initialise(static_cast<int32_t>(i), sampleRate);
            ApplyCurrentParametersToProcessors();
        }
    }

    void Reset()
    {
        for (auto& proc : processors) {
            proc->reset();
        }

        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            pendingBuffers.assign(bufferEndpoints.size(), nullptr);
            activeBuffers.assign(bufferEndpoints.size(), nullptr);
        }

        ApplyCurrentParametersToProcessors();
    }

    void Process(const float* inBuffer, float* outBuffer, unsigned int length, int inChannels, int outChannels)
    {
        ApplyPendingBuffers();

        if (multiChannelExpandable && inChannels > 1)
        {
            size_t processChans = std::min((size_t)inChannels, processors.size());
            
            if (inChannels != lastChannelCount || length != lastLength)
            {
                lastChannelCount = inChannels;
                lastLength = length;
                
                if (deInterleaveBuffer) delete[] deInterleaveBuffer;
                if (interleaveBuffer) delete[] interleaveBuffer;
                
                deInterleaveBuffer = new float[length * inChannels];
                interleaveBuffer = new float[length * inChannels];
            }
            
            // De-interleave
            if (inBuffer) {
                for (size_t i = 0; i < length; i++) {
                    for (size_t c = 0; c < inChannels; c++) {
                        deInterleaveBuffer[c * length + i] = inBuffer[inChannels * i + c];
                    }
                }
            }
            
            // Process each channel
            for (size_t i = 0; i < processChans; i++) {
                if (inBuffer) {
                    processors[i]->setInputFrames(audioInputHandle, &deInterleaveBuffer[i * length], length, 0);
                }
                
                processors[i]->advance(length);
                
                if (outBuffer) {
                    processors[i]->copyOutputFrames(audioOutputHandle, &interleaveBuffer[i * length], length);
                }
            }
            
            // Clear remaining channels in interleave buffer if any (silence)
            if (processChans < inChannels && outBuffer) {
                size_t processedSamples = processChans * length;
                size_t totalSamples = inChannels * length;
                memset(interleaveBuffer + processedSamples, 0, (totalSamples - processedSamples) * sizeof(float));
            }
            
            // Interleave
            if (outBuffer) {
                for (size_t c = 0; c < inChannels; c++) {
                    for (size_t i = 0; i < length; i++) {
                        outBuffer[c + inChannels * i] = interleaveBuffer[c * length + i];
                    }
                }
            }
        }
        else
        {
            if (inBuffer && numInputChannels > 0) {
                processors[0]->setInputFrames(audioInputHandle, inBuffer, length, 0);
            }
            
            processors[0]->advance(length);
            
            if (outBuffer && numOutputChannels > 0) {
                processors[0]->copyOutputFrames(audioOutputHandle, outBuffer, length);
            }
        }
    }

    bool SetParameterValue(int index, float value)
    {
        if (index < 0 || index >= static_cast<int>(parameters.size()))
            return false;
        auto& param = parameters[static_cast<size_t>(index)];
        param.currentValue = value;
        for (auto& proc : processors) {
            proc->addEvent(param.handle, 0, reinterpret_cast<const unsigned char*>(&value));
        }
        return true;
    }

    int GetFloatParameterCount() const
    {
        return static_cast<int>(parameters.size());
    }

    size_t GetBufferEndpointCount() const
    {
        return bufferEndpoints.size();
    }

    bool IsBufferDataParameterIndex(int index) const
    {
        const int firstDataIndex = GetFloatParameterCount();
        const int lastDataIndexExclusive = firstDataIndex + static_cast<int>(bufferEndpoints.size());
        return index >= firstDataIndex && index < lastDataIndexExclusive;
    }

    const BufferEndpointInfo* TryGetBufferEndpointForDataParameter(int index) const
    {
        if (!IsBufferDataParameterIndex(index))
            return nullptr;

        const size_t endpointIndex = static_cast<size_t>(index - GetFloatParameterCount());
        return endpointIndex < bufferEndpoints.size() ? &bufferEndpoints[endpointIndex] : nullptr;
    }

    BufferLoadResult SetDataParameterBuffer(int index, const void* data, size_t dataLength)
    {
        if (!IsBufferDataParameterIndex(index))
            return BufferLoadResult::NotDataParameter;

        if (!ProcessorSupportsExternalBufferDelivery())
            return BufferLoadResult::UnsupportedEndpointType;

        std::vector<float> decodedSamples;
        unsigned int channels = 0;
        unsigned int sampleRate = 0;
        if (!DecodeAudio(data, dataLength, decodedSamples, channels, sampleRate))
            return BufferLoadResult::DecodeFailed;

        if (channels == 0 || decodedSamples.empty())
            return BufferLoadResult::DecodeFailed;

        const size_t endpointIndex = static_cast<size_t>(index - GetFloatParameterCount());
        if (endpointIndex >= bufferEndpoints.size())
            return BufferLoadResult::NotDataParameter;

        std::vector<float> monoSamples;
        if (channels == 1) {
            monoSamples = std::move(decodedSamples);
        } else {
            const size_t frameCount = decodedSamples.size() / channels;
            monoSamples.resize(frameCount);
            for (size_t frame = 0; frame < frameCount; ++frame) {
                float sum = 0.0f;
                for (unsigned int ch = 0; ch < channels; ++ch) {
                    sum += decodedSamples[(frame * channels) + ch];
                }
                monoSamples[frame] = sum / static_cast<float>(channels);
            }
        }

        auto bufferData = std::make_shared<ExternalBufferData>();
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            bufferData->samples = std::move(monoSamples);
            bufferData->channels = 1;
            bufferData->sampleRate = sampleRate;
            bufferData->frameCount = bufferData->samples.size();
            pendingBuffers[endpointIndex] = std::move(bufferData);
        }

        return BufferLoadResult::Queued;
    }

    // Last value set via dspsetparamfloat / automation (Cmajor is updated via addEvent; there is no generic readback from the processor).
    bool TryGetParameterFloat(int index, float* outValue) const
    {
        if (!outValue || index < 0 || index >= static_cast<int>(parameters.size()))
            return false;
        *outValue = parameters[static_cast<size_t>(index)].currentValue;
        return true;
    }

    void ApplyCurrentParametersToProcessors()
    {
        for (const auto& param : parameters) {
            for (auto& proc : processors) {
                proc->addEvent(param.handle, 0, reinterpret_cast<const unsigned char*>(&param.currentValue));
            }
        }
    }

    struct ParamInfo {
        std::string id;
        std::string name;
        float min;
        float max;
        float init;
        std::string unit;
        uint32_t handle;
        float currentValue = 0.0f;
    };

    struct ExternalBufferData {
        std::vector<float> samples;
        unsigned int channels = 0;
        unsigned int sampleRate = 0;
        size_t frameCount = 0;
    };

    static bool IsBufferLoadableInputEndpoint(const json& input)
    {
        if (!input.contains("endpointType") || input["endpointType"] != "event")
            return false;

        if (!input.contains("annotation"))
            return false;

        const auto& ann = input["annotation"];
        const bool flagged = JsonAnnotationTrue(ann, "fmodBuffer");
        if (!flagged || !input.contains("endpointID"))
            return false;

        const auto endpointID = input["endpointID"].template get<std::string>();
        return IsSupportedBufferEndpointID(endpointID);
    }

    static bool TryParseBufferEndpointID(const std::string& endpointID, int& outIndex)
    {
        static const std::regex pattern("^buf([0-9]+)$");
        std::smatch match;
        if (!std::regex_match(endpointID, match, pattern) || match.size() != 2 || match[1].str().empty())
            return false;

        try {
            const int index = std::stoi(match[1].str());
            if (index < 0)
                return false;
            outIndex = index;
            return true;
        } catch (...) {
            return false;
        }
    }

    static bool IsSupportedBufferEndpointID(const std::string& endpointID)
    {
        int ignored = 0;
        return TryParseBufferEndpointID(endpointID, ignored);
    }

    static bool JsonAnnotationTrue(const json& ann, const char* key)
    {
        if (!ann.contains(key))
            return false;

        const auto& value = ann[key];
        if (value.is_boolean())
            return value.template get<bool>();
        if (value.is_number_integer())
            return value.template get<int>() != 0;
        if (value.is_string()) {
            auto text = value.template get<std::string>();
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text == "true" || text == "1" || text == "yes";
        }
        return false;
    }

    bool DecodeAudio(const void* data, size_t dataLength, std::vector<float>& decoded, unsigned int& channels, unsigned int& sampleRate)
    {
        drwav wav;
        if (drwav_init_memory(&wav, data, dataLength, nullptr)) {
            channels = wav.channels;
            sampleRate = wav.sampleRate;
            const drwav_uint64 frameCount = wav.totalPCMFrameCount;
            const drwav_uint64 totalSampleCount = frameCount * channels;
            decoded.resize(static_cast<size_t>(totalSampleCount));
            drwav_read_pcm_frames_f32(&wav, frameCount, decoded.data());
            drwav_uninit(&wav);
            return true;
        }

        drmp3 mp3;
        if (drmp3_init_memory(&mp3, data, dataLength, nullptr)) {
            channels = mp3.channels;
            sampleRate = mp3.sampleRate;
            const drmp3_uint64 frameCount = drmp3_get_pcm_frame_count(&mp3);
            const drmp3_uint64 totalSampleCount = frameCount * channels;
            decoded.resize(static_cast<size_t>(totalSampleCount));
            drmp3_read_pcm_frames_f32(&mp3, frameCount, decoded.data());
            drmp3_uninit(&mp3);
            return true;
        }
        return false;
    }

    static constexpr bool ProcessorSupportsExternalBufferDelivery()
    {
        return requires(Processor& proc, uint32_t handle, const unsigned char* data) {
            typename Processor::std_audio_data_Mono {};
            proc.addEvent(handle, 0, data);
        };
    }

    // cmaj-generated generic addEvent(handle, blob) can mis-deserialize Mono payloads,
    // so we dispatch via typed addEvent_buf0..addEvent_bufN.
#define CMAJOR_BUF_INDEX_LIST(X) \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) \
    X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) \
    X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) \
    X(30) X(31)

    template <typename Proc>
    static bool DispatchBufIndex(Proc& proc, int index, const typename Proc::std_audio_data_Mono& e)
    {
        switch (index) {
#define CMAJOR_BUF_CASE(NUM)                                                    \
            case NUM:                                                           \
                if constexpr (requires { proc.addEvent_buf##NUM(e); }) {        \
                    proc.addEvent_buf##NUM(e);                                  \
                    return true;                                                \
                }                                                               \
                return false;
            CMAJOR_BUF_INDEX_LIST(CMAJOR_BUF_CASE)
#undef CMAJOR_BUF_CASE
            default:
                return false;
        }
    }

    template <typename Proc>
    static bool TryDeliverMonoBufferEventByEndpointId(Proc& proc, const std::string& endpointId, const typename Proc::std_audio_data_Mono& e)
    {
        int index = 0;
        if (!TryParseBufferEndpointID(endpointId, index))
            return false;

        return DispatchBufIndex(proc, index, e);
    }
#undef CMAJOR_BUF_INDEX_LIST

    template <typename Proc>
    static void DeliverBufferToProcessor(Proc& proc, uint32_t endpointHandle, const ExternalBufferData& buffer, const std::string& endpointId)
    {
        typename Proc::std_audio_data_Mono eventData {};
        eventData.frames = { const_cast<float*>(buffer.samples.data()), static_cast<typename Proc::SizeType>(buffer.samples.size()) };
        eventData.sampleRate = static_cast<double>(buffer.sampleRate);

        (void) endpointHandle;
        TryDeliverMonoBufferEventByEndpointId(proc, endpointId, eventData);
    }

    void ApplyPendingBuffers()
    {
        std::vector<std::shared_ptr<ExternalBufferData>> updates;
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            updates = pendingBuffers;
            pendingBuffers.assign(bufferEndpoints.size(), nullptr);
        }

        if (!updates.empty()) {
            if constexpr (ProcessorSupportsExternalBufferDelivery()) {
                for (size_t endpointIndex = 0; endpointIndex < updates.size(); ++endpointIndex) {
                    if (!updates[endpointIndex])
                        continue;

                    const auto& endpoint = bufferEndpoints[endpointIndex];
                    for (auto& proc : processors) {
                        DeliverBufferToProcessor(*proc, endpoint.handle, *updates[endpointIndex], endpoint.id);
                    }
                }

                std::lock_guard<std::mutex> lock(bufferMutex);
                activeBuffers = std::move(updates);
            }
        }
    }

    std::vector<std::unique_ptr<Processor>> processors;
    double sampleRate = 44100.0;
    std::vector<ParamInfo> parameters;
    std::vector<BufferEndpointInfo> bufferEndpoints;
    int numInputChannels = 0;
    int numOutputChannels = 0;
    uint32_t audioInputHandle = 0;
    uint32_t audioOutputHandle = 0;
    
    bool multiChannelExpandable = false;
    float* deInterleaveBuffer = nullptr;
    float* interleaveBuffer = nullptr;
    size_t lastChannelCount = 0;
    size_t lastLength = 0;

    unsigned int lastHostBlockFrames = 0;
    std::vector<std::shared_ptr<ExternalBufferData>> pendingBuffers;
    std::vector<std::shared_ptr<ExternalBufferData>> activeBuffers;
    std::mutex bufferMutex;
};
