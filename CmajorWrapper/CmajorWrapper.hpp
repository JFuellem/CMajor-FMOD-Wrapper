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

    CmajorWrapper();
    ~CmajorWrapper();

    void Init(double sampleRate);
    void InitWithHost(double sampleRate, int hostBlockFrames);
    void InitMetadata();

    void SyncFromFmod(FMOD_DSP_STATE* dsp_state);
    void Reset();

    void Process(const float* inBuffer, float* outBuffer, unsigned int length, int inChannels, int outChannels);

    bool SetParameterValue(int index, float value);

    int GetFloatParameterCount() const;
    size_t GetBufferEndpointCount() const;

    bool IsBufferDataParameterIndex(int index) const;
    const BufferEndpointInfo* TryGetBufferEndpointForDataParameter(int index) const;

    BufferLoadResult SetDataParameterBuffer(int index, const void* data, size_t dataLength);

    bool TryGetParameterFloat(int index, float* outValue) const;

    void ApplyCurrentParametersToProcessors();

    static bool IsBufferLoadableInputEndpoint(const json& input);
    static bool TryParseBufferEndpointID(const std::string& endpointID, int& outIndex);
    static bool IsSupportedBufferEndpointID(const std::string& endpointID);
    static bool JsonAnnotationTrue(const json& ann, const char* key);

    bool DecodeAudio(const void* data, size_t dataLength, std::vector<float>& decoded, unsigned int& channels, unsigned int& sampleRate);

    static constexpr bool ProcessorSupportsExternalBufferDelivery();

    template <typename Proc>
    static bool DispatchBufIndex(Proc& proc, int index, const typename Proc::std_audio_data_Mono& e);

    template <typename Proc>
    static bool TryDeliverMonoBufferEventByEndpointId(Proc& proc, const std::string& endpointId, const typename Proc::std_audio_data_Mono& e);

    template <typename Proc>
    static void DeliverBufferToProcessor(Proc& proc, uint32_t endpointHandle, const ExternalBufferData& buffer, const std::string& endpointId);

    void ApplyPendingBuffers();

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

#include "CmajorWrapper.inl"
