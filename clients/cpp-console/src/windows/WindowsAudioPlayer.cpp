// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <cstring>
#include <chrono>
#include <condition_variable>
#include <string>
#include <thread>
#include <mutex>
#include <atlbase.h>
#include <avrt.h>
#include "WindowsAudioPlayer.h"

using namespace AudioPlayer;

#define ENGINE_LATENCY_IN_MSEC 1000

void SAFE_CLOSEHANDLE(HANDLE _handle_) {
    if (NULL != _handle_)
    {
        CloseHandle(_handle_);
        _handle_ = NULL;
    }
}

WindowsAudioPlayer::WindowsAudioPlayer() {
    m_hAudioClientEvent = NULL;
    m_hRenderThread = NULL;
    m_hStartEvent = NULL;
    m_hStopEvent = NULL;
    m_hRenderingDoneEvent = NULL;
    m_pAudioClient = NULL;
    m_pRenderClient = NULL;
    m_renderBuffer = NULL;

    m_renderBufferOffsetInFrames = 0;
    m_renderBufferSizeInFrames = 0;
    m_loopRenderBufferFlag = false; //by default looping render buffer is off
    m_muteChannelMask = 0;
    if (CoInitialize(nullptr) != S_OK) {
        fprintf(stdout, "CoInitialize failed\n");
    }
    Open();
    m_playerThread = std::thread(&WindowsAudioPlayer::PlayerThreadMain, this);
}

WindowsAudioPlayer::~WindowsAudioPlayer() {
    Close();
    m_canceled = true;
    m_threadMutex.unlock();
    m_conditionVariable.notify_one();
    m_playerThread.join();

    CoUninitialize();
    SAFE_CLOSEHANDLE(m_hAudioClientEvent);
    SAFE_RELEASE(m_pRenderClient);
    SAFE_RELEASE(m_pAudioClient);
}

int WindowsAudioPlayer::Open() {
    int rc;
    rc = Open("default", AudioPlayerFormat::Mono16khz16bit);
    return rc;
}

int WindowsAudioPlayer::Open(const std::string& device, AudioPlayerFormat format) {
    HRESULT hr = S_OK;
    CComPtr<IMMDeviceEnumerator> pEnumerator;
    CComPtr<IMMDevice> pDevice;
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_MILLISEC * ENGINE_LATENCY_IN_MSEC;

    switch (format) {
    case AudioPlayerFormat::Mono16khz16bit:
    default:
        /* Signed 16-bit little-endian format */
        fprintf(stdout, "Format = Mono16khz16bit\n");
        m_pwf.nChannels = 1;
        m_pwf.nBlockAlign = 2;
        m_pwf.wBitsPerSample = 16;
        m_pwf.nSamplesPerSec =16000;
        m_pwf.nAvgBytesPerSec = m_pwf.nSamplesPerSec * m_pwf.nBlockAlign;
        m_pwf.cbSize = 0;
        m_pwf.wFormatTag = WAVE_FORMAT_PCM;
    }

    //Begin Audio Device Setup
    CComCritSecLock<CComAutoCriticalSection> lock(m_cs);

    // get a device enumator from the OS
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&pEnumerator);
    if (hr != S_OK) {
        goto exit;
    }

    // use the enumerator to get the default device
    hr = pEnumerator->GetDefaultAudioEndpoint(
        eRender, eConsole, &pDevice);
    if (hr != S_OK) {
        goto exit;
    }

    // use the device to activate an AudioClient
    hr = pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL,
        NULL, (void**)&m_pAudioClient);
    if (hr != S_OK) {
        goto exit;
    }

    hr = m_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
        hnsRequestedDuration,
        0,
        &m_pwf,
        NULL);
    // Note: AUDCLNT_SHAREMODE_EXCLUSIVE is not supported in Durango
    if (hr != S_OK) {
        goto exit;
    }

    hr = m_pAudioClient->GetService(
    _uuidof(IAudioRenderClient),
    (void**)&m_pRenderClient);
    if (hr != S_OK) {
        goto exit;
    }
    hr = m_pAudioClient->Start();

exit:

    return hr;
}

void WindowsAudioPlayer::PlayerThreadMain() {
    HRESULT hr = S_OK;
    m_canceled = false;

    while (m_canceled == false) {
        // here we will wait to be woken up since there is no audio left to play
        std::unique_lock<std::mutex> lk{ m_threadMutex };
        m_conditionVariable.wait(lk);
        lk.unlock();

        while (m_audioQueue.size() > 0) {
            m_isPlaying = true;
            std::shared_ptr<AudioPlayerEntry> entry = std::make_shared<AudioPlayerEntry>(m_audioQueue.front());
            switch(entry->m_entryType){
                case PlayerEntryType::BYTE_ARRAY:
                    PlayByteBuffer(entry);
                    break;
                case PlayerEntryType::PULL_AUDIO_OUTPUT_STREM:
                    PlayPullAudioOutputStream(entry);
                    break;
                default:
                    fprintf(stderr, "Unknown Audio Player Entry type\n");   
            }
            m_queueMutex.lock();
            m_audioQueue.pop_front();
            m_queueMutex.unlock();
        }
        m_isPlaying = false;
    }
}

void WindowsAudioPlayer::PlayPullAudioOutputStream(std::shared_ptr<AudioPlayerEntry> pEntry){
    HRESULT hr = S_OK;
    UINT32 maxBufferSizeInFrames = 0;
    UINT32 paddingFrames;
    UINT32 framesAvailable;
    UINT32 framesToWrite;
    BYTE* pData, *pStreamData;
    unsigned int bytesRead = 0;
    std::shared_ptr<Audio::PullAudioOutputStream> stream = pEntry->m_pullStream;

    hr = m_pAudioClient->GetBufferSize(&maxBufferSizeInFrames);
    if (FAILED(hr)) {
        fprintf(stderr, "Error. Failed to GetBufferSize. Error: 0x%08x\n", hr);
        return;
    }

    do{
        hr = m_pAudioClient->GetCurrentPadding(&paddingFrames);
        if(FAILED(hr)){
            fprintf(stderr, "Error. Failed to GetCurrentPadding. Error: 0x%08x\n", hr);
            continue;
        }

        framesAvailable = maxBufferSizeInFrames - paddingFrames;
        UINT32 sizeToWrite = framesAvailable * m_pwf.nBlockAlign;

        if (sizeToWrite == 0) {
            //this means the buffer is full so we will wait till some room opens up
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        pStreamData = (BYTE*) malloc(sizeToWrite);
        bytesRead = stream->Read(pStreamData, sizeToWrite);

        if (sizeToWrite > bytesRead) {
            sizeToWrite = (UINT32)bytesRead;
        }
        framesToWrite = sizeToWrite / m_pwf.nBlockAlign;

        hr = m_pRenderClient->GetBuffer(framesToWrite, &pData);
        if (FAILED(hr)) {
            fprintf(stderr, "Error. Failed to GetBuffer. Error: 0x%08x\n", hr);
            goto freeBeforeContinue;
        }

        memcpy_s(pData, sizeToWrite, pStreamData, sizeToWrite);

        hr = m_pRenderClient->ReleaseBuffer(framesToWrite, 0);
        if (FAILED(hr))
        {
            printf("Error. Failed to ReleaseBuffer. Error: 0x%08x\n", hr);
            goto freeBeforeContinue;
        }

freeBeforeContinue:
        free(pStreamData);
    } while(bytesRead > 0);

}

void WindowsAudioPlayer::PlayByteBuffer(std::shared_ptr<AudioPlayerEntry> pEntry){
    HRESULT hr = S_OK;
    UINT32 maxBufferSizeInFrames = 0;
    UINT32 paddingFrames;
    UINT32 framesAvailable;
    UINT32 framesToWrite;
    BYTE* pData;
    size_t bufferLeft = pEntry->m_size;

    hr = m_pAudioClient->GetBufferSize(&maxBufferSizeInFrames);
    if (FAILED(hr)) {
        fprintf(stderr, "Error. Failed to GetBufferSize. Error: 0x%08x\n", hr);
        return;
    }

    while (bufferLeft > 0) {
        

        //
        //  We want to find out how much of the buffer *isn't* available (is padding).
        //

        hr = m_pAudioClient->GetCurrentPadding(&paddingFrames);
        if(FAILED(hr)){
            fprintf(stderr, "Error. Failed to GetCurrentPadding. Error: 0x%08x\n", hr);
            continue;
        }

        framesAvailable = maxBufferSizeInFrames - paddingFrames;
        UINT32 sizeToWrite = framesAvailable * m_pwf.nBlockAlign;

        if (sizeToWrite > bufferLeft) {
            sizeToWrite = (UINT32)bufferLeft;
        }

        framesToWrite = sizeToWrite / m_pwf.nBlockAlign;
        hr = m_pRenderClient->GetBuffer(framesToWrite, &pData);
        if (FAILED(hr)) {
            fprintf(stderr, "Error. Failed to GetBuffer. Error: 0x%08x\n", hr);
            continue;
        }

        memcpy_s(pData, sizeToWrite, &pEntry->m_data[pEntry->m_size - bufferLeft], sizeToWrite);

        bufferLeft -= sizeToWrite;

        hr = m_pRenderClient->ReleaseBuffer(framesToWrite, 0);
        if (FAILED(hr))
        {
            printf("Error. Failed to ReleaseBuffer. Error: 0x%08x\n", hr);
            continue;
        }
    }
}

int WindowsAudioPlayer::Play(uint8_t* buffer, size_t bufferSize) {
    int rc = 0;
    AudioPlayerEntry entry(buffer, bufferSize);
    m_queueMutex.lock();
    m_audioQueue.push_back(entry);
    m_queueMutex.unlock();

    if (!m_isPlaying) {
        //wake up the audio thread
        m_conditionVariable.notify_one();
    }

    return rc;
}

int WindowsAudioPlayer::Play(std::shared_ptr<Microsoft::CognitiveServices::Speech::Audio::PullAudioOutputStream> pStream) {
    int rc = 0;
    AudioPlayerEntry entry(pStream);
    m_queueMutex.lock();
    m_audioQueue.push_back(entry);
    m_queueMutex.unlock();

    if (!m_isPlaying) {
        //wake up the audio thread
        m_conditionVariable.notify_one();
    }

    return rc;
}

int WindowsAudioPlayer::SetVolume(unsigned int percent){
    return 0;
}

int WindowsAudioPlayer::Close() {

    return 0;
}