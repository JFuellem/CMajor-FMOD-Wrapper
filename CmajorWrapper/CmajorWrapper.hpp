#pragma once

#include "fmod.h"
#include "fmod_dsp.h"
#include "json.hpp"
#include <vector>
#include <string>
#include <memory>
#include <map>

using json = nlohmann::json;

template <typename Processor>
class CmajorWrapper
{
public:
    CmajorWrapper()
    {
    }

    ~CmajorWrapper()
    {
        if (deInterleaveBuffer) delete[] deInterleaveBuffer;
        if (interleaveBuffer) delete[] interleaveBuffer;
    }

    void Init(double sampleRate)
    {
        this->sampleRate = sampleRate;
        
        // Parse JSON for parameter info from a temporary processor
        Processor tempProcessor;
        auto j = json::parse(Processor::programDetailsJSON);
        
        if (j.contains("inputs")) {
            for (const auto& input : j["inputs"]) {
                if (input["endpointType"] == "event" && input["dataType"]["type"] == "float32") {
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
                    
                    parameters.push_back(info);
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

        multiChannelExpandable = (numInputChannels == 1 && numOutputChannels == 1);

        size_t c = 0;
        do {
            processors.push_back(std::make_unique<Processor>());
            processors.back()->initialise(c, sampleRate);
            c++;
        } while (multiChannelExpandable && c < 32);
    }

    void Reset()
    {
        for (auto& proc : processors) {
            proc->reset();
        }
    }

    void Process(const float* inBuffer, float* outBuffer, unsigned int length, int inChannels, int outChannels)
    {
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

    void SetParameterValue(int index, float value)
    {
        if (index >= 0 && index < parameters.size()) {
            auto& param = parameters[index];
            param.currentValue = value;
            for (auto& proc : processors) {
                proc->addEvent(param.handle, 0, reinterpret_cast<const unsigned char*>(&value));
            }
        }
    }

    float GetParameterValue(int index)
    {
        if (index >= 0 && index < parameters.size()) {
            return parameters[index].currentValue;
        }
        return 0.0f;
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

    std::vector<std::unique_ptr<Processor>> processors;
    double sampleRate = 44100.0;
    std::vector<ParamInfo> parameters;
    int numInputChannels = 0;
    int numOutputChannels = 0;
    uint32_t audioInputHandle = 0;
    uint32_t audioOutputHandle = 0;
    
    bool multiChannelExpandable = false;
    float* deInterleaveBuffer = nullptr;
    float* interleaveBuffer = nullptr;
    size_t lastChannelCount = 0;
    size_t lastLength = 0;
};
