//
//  RNBOWrapper.hpp
//  RNBO_FMOD
//
//  Created by Jonas Füllemann on 19.01.24.
//

#ifndef RNBOWrapper_hpp
#define RNBOWrapper_hpp

#define RNBO_SIMPLEENGINE

#include <stdio.h>
#include <vector>
#include <map>

#include "RNBO.h"
#include "fmod.hpp"

#ifdef RNBO_LEGACY_PATCHER_FACTORY
namespace RNBO {
class PlatformInterface;
}
#endif
#include "dr_wav.h"
#include "dr_mp3.h"

#define MAX_CHANS 12

class RNBOWrapper
{
public:
    std::vector<std::unique_ptr<RNBO::CoreObject>> rnboObj;
    size_t isInstrument = 1;
    FMOD_BOOL inputsIdle = 0;
    FMOD_BOOL lastIdleState = 0;
    bool shouldGoIdle = false;
    size_t timeStore = 0;
    size_t tailLength = 0;
    int sampleRate = 48000;
    
    bool multiChannelExpandable = false;
    float* deInterleaveBuffer = nullptr;
    float* interleaveBuffer = nullptr;
    size_t lastChannelCount = -1;
    
    std::map<size_t, float*> mDataRefBuffers;

    // Destructor to ensure proper cleanup
    ~RNBOWrapper() {
        CleanupBuffers();
    }

#ifdef RNBO_LEGACY_PATCHER_FACTORY
    void Init(RNBO::PatcherFactoryFunctionPtr (*factoryProvider)(RNBO::PlatformInterface*));
#else
    void Init(RNBO::PatcherFactoryFunctionPtr (*factoryProvider)());
#endif
    void Reset();
    void CleanupBuffers();
    bool DecodeAudio(const void* data, size_t dataLength, char*& decodedData, size_t& decodedLengthInBytes, unsigned int& channels, unsigned int& sampleRate);
    void SetExternalData(size_t dataRefIndex, char* data, size_t sizeInBytes, unsigned int channels, unsigned int sampleRate);


};

namespace RNBOFMODHelpers {

bool CheckRNBOTag(const RNBO::CoreObject &obj, const char* tag);

bool CheckAnyRNBOSystemTag(const RNBO::CoreObject &obj);

void DispatchRNBOMessages(RNBO::CoreObject *obj, void* rawData);

FMOD_SPEAKERMODE GetSpeakermode(const RNBO::Index& channels);
    
bool CheckIfOutputQuiet(float* outarray,size_t buffsize, size_t numchans);

}



#endif /* RNBOWrapper_hpp */
