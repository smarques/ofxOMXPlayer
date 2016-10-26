#include "ofxOMXPlayerEngine.h"


#if OMX_LOG_LEVEL > OMX_LOG_LEVEL_ERROR_ONLY
#define ENGINE_LOG(x)  ofLogVerbose(__func__) << ofToString(x);
#else
#define ENGINE_LOG(x)
#endif

#define LOOP_LOG(x) 
//#define LOOP_LOG(x)  ofLogVerbose(__func__) << __LINE__ << x;

ofxOMXPlayerEngine::ofxOMXPlayerEngine()
{
    
    videoWidth = 0;
    videoHeight = 0;
    hasVideo = false;
    hasAudio = false;
    packet = NULL;
    moviePath = "moviePath is undefined";
    
    bPlaying = false;
    duration = 0.0;
    nFrames = 0;
    
    useHDMIForAudio = true;
    loop_offset = 0;
    startpts = 0.0;
    
    didAudioOpen = false;
    didVideoOpen = false;
    isTextureEnabled = false;
    doLooping = false;
    videoPlayer = NULL;
    texturedPlayer = NULL;
    directPlayer = NULL;
    audioPlayer = NULL;
    omxClock = NULL;
    
    listener = NULL;
    
    loopCounter = 0;
    previousLoopOffset = -1;
        
    normalPlaySpeed = 1000;
    speedMultiplier = 1;
    doSeek = false;
    
    eglImage = NULL;
    doRestart = false;
    frameCounter = 0;
    loopFrame = 0;
}

#pragma mark startup/setup

bool ofxOMXPlayerEngine::setup(ofxOMXPlayerSettings& settings)
{
    omxPlayerSettings = settings;
    moviePath = omxPlayerSettings.videoPath;
    useHDMIForAudio = omxPlayerSettings.useHDMIForAudio;
    doLooping = omxPlayerSettings.enableLooping;
    addListener(omxPlayerSettings.listener);
    
    //ofLogVerbose(__func__) << "moviePath is " << moviePath;
    isTextureEnabled = omxPlayerSettings.enableTexture;
    
    bool doSkipAvProbe = true;
    bool didOpenMovie = didReadFile(doSkipAvProbe);
    
    if(!didOpenMovie)
    {
        ofLogWarning(__func__) << "FAST PATH MOVE OPEN FAILED - LIKELY A STREAM, TRYING SLOW PATH";
        doSkipAvProbe = false;
        didOpenMovie = didReadFile(doSkipAvProbe);
    }
    
    if(didOpenMovie)
    {
        
        //ofLogVerbose(__func__) << "omxReader open moviePath PASS: " << moviePath;
        
        hasVideo = omxReader.getNumVideoStreams();
        int audioStreamCount = omxReader.getNumAudioStreams();
        
        if (audioStreamCount>0)
        {
            hasAudio = true;
            omxReader.getHints(OMXSTREAM_AUDIO, audioStreamInfo);
            //ofLogVerbose(__func__) << "HAS AUDIO";
        }
        else
        {
            //ofLogVerbose(__func__) << "NO AUDIO";
        }
        
        if (!omxPlayerSettings.enableAudio)
        {
            hasAudio = false;
        }
        
        if (hasVideo)
        {
            
            //again?
            omxReader.getHints(OMXSTREAM_VIDEO, videoStreamInfo);
            
            videoWidth = videoStreamInfo.width;
            videoHeight = videoStreamInfo.height;
            omxPlayerSettings.videoWidth = videoStreamInfo.width;
            omxPlayerSettings.videoHeight = videoStreamInfo.height;
            
            //ofLogVerbose(__func__) << "SET videoWidth: " << videoWidth;
            //ofLogVerbose(__func__) << "SET videoHeight: " << videoHeight;
            //ofLogVerbose(__func__) << "videoStreamInfo.nb_frames " <<videoStreamInfo.nb_frames;
            if(omxClock)
            {
                delete omxClock;
                omxClock = NULL;
            }
            omxClock = new OMXClock();
            
            if(omxClock->init(hasVideo, hasAudio))
            {
                //ofLogVerbose(__func__) << "omxClock Init PASS";
                return true;
            }
            else
            {
                ofLogError() << "omxClock Init FAIL";
                return false;
            }
        }
        else
        {
            ofLogError() << "Video streams detection FAIL";
            return false;
        }
    }
    else
    {
        ofLogError() << "omxReader open moviePath FAIL: " << moviePath;
        return false;
    }
}


void ofxOMXPlayerEngine::enableLooping()
{
    lock();
        doLooping = true;
    unlock();
}

void ofxOMXPlayerEngine::disableLooping()
{
    lock();
        doLooping = false;
    unlock();
}


bool ofxOMXPlayerEngine::didReadFile(bool doSkipAvProbe)
{
    bool passed = false;
    
    unsigned long long startTime = ofGetElapsedTimeMillis();
    
    bool didOpenMovie = omxReader.open(moviePath.c_str(), doSkipAvProbe);
    
    unsigned long long endTime = ofGetElapsedTimeMillis();
    ofLogNotice(__func__) << "didOpenMovie TOOK " << endTime-startTime << " MS";
    
    
    if(didOpenMovie)
    {
        omxReader.getHints(OMXSTREAM_VIDEO, videoStreamInfo);
        if(videoStreamInfo.width > 0 || videoStreamInfo.height > 0)
        {
            passed = true;
        }
    }
    return passed;
}


bool ofxOMXPlayerEngine::openPlayer(int startTimeInSeconds)
{
    
    if (isTextureEnabled)
    {
        if (!texturedPlayer)
        {
            texturedPlayer = new VideoPlayerTextured();
        }
        didVideoOpen = texturedPlayer->open(videoStreamInfo, omxClock, &omxReader, omxPlayerSettings, eglImage);
        videoPlayer = (BaseVideoPlayer*)texturedPlayer;
    }
    else
    {
        if (!directPlayer)
        {
            directPlayer = new VideoPlayerDirect();
        }
 
        didVideoOpen = directPlayer->open(videoStreamInfo, omxClock, &omxReader, omxPlayerSettings);
        videoPlayer = (BaseVideoPlayer*)directPlayer;
    }
    
    bPlaying = didVideoOpen;
    
    if (hasAudio)
    {
        string deviceString = "omx:hdmi";
        if (!useHDMIForAudio)
        {
            deviceString = "omx:local";
        }
        audioPlayer = new OMXAudioPlayer();
        didAudioOpen = audioPlayer->open(audioStreamInfo, omxClock, &omxReader, deviceString);
        if (didAudioOpen)
        {
            setVolume(omxPlayerSettings.initialVolume);
        }
        else
        {
            ofLogError(__func__) << " AUDIO PLAYER OPEN FAIL";
        }
    }
    
    
    if (isPlaying())
    {
        
        
        if(videoStreamInfo.nb_frames>0 && videoPlayer->getFPS()>0)
        {
            nFrames =videoStreamInfo.nb_frames;
            duration =videoStreamInfo.nb_frames / videoPlayer->getFPS();
            ofLogNotice(__func__) << "duration SET: " << duration;
        }
        else
        {
            
            //file is weird (like test.h264) and has no reported frames
        }
        if (startTimeInSeconds>0 && omxReader.canSeek())
        {
            
            bool didSeek = omxReader.SeekTime(startTimeInSeconds * 1000.0f, 0, &startpts);
            if(!didSeek)
            {
                ofLogError(__func__) << "COULD NOT SEEK TO " << startTimeInSeconds;
            }
        }
        omxClock->start(startpts, getFPS());
        
        ENGINE_LOG("Opened video PASS");
        startThread();
        
        
        return true;
    }
    else
    {
        ofLogError(__func__) << "Opened video FAIL";
        return false;
    }
}


void ofxOMXPlayerEngine::sendOnLoop(int lineNumber)
{
    ofLogVerbose(__func__) << "FROM " << lineNumber;
    lock();
    loopCounter++;
    frameCounter = 0;
    if(directPlayer)
    {
        omxClock->getMediaTime();
    }
    frameCounter = omxClock->getFrameCounter();    
    
    if (listener != NULL)
    {
        
        ofxOMXPlayerListenerEventData eventData((void *)this);
        listener->onVideoLoop(eventData);
    }
    unlock();
}

void ofxOMXPlayerEngine::seekToStart()
{
    lock();
    ENGINE_LOG("SEEKED");
    omxReader.SeekTime(0 * 1000.0f, AVSEEK_FLAG_BACKWARD, &startpts);
    
    packet = omxReader.Read();
    
    if(hasAudio)
    {
        loop_offset = audioPlayer->getCurrentPTS();
    }
    else
    {
        if(hasVideo)
        {
            loop_offset = videoPlayer->getCurrentPTS();
        }
    }
    unlock();
}
#define SLEEP_TIME 10
#define THREAD_FUNC sleep
//#define THREAD_FUNC omxClock->sleep

#define THREAD_SLEEP THREAD_FUNC(SLEEP_TIME);
#pragma mark threading
void ofxOMXPlayerEngine::threadedFunction()
{
    while (isThreadRunning())
    {
        
        
        
        updateCurrentFrame();
       // ofLogVerbose(__func__) << omxReader.packetsAllocated << " packetsFreed: " << omxReader.packetsFreed << " leaked: " << (omxReader.packetsAllocated-omxReader.packetsFreed);
        //ofLogVerbose(__func__) << " remaining packets: " << remainingPackets;
        //ofLogVerbose(__func__) << __LINE__ << " " << getCurrentFrame() << " of " << getTotalNumFrames();
        if(doLooping && !omxReader.isStream)
        {

            int currentFrame = getCurrentFrame();
            
            if(currentFrame && currentFrame>=getTotalNumFrames())
            {
                //ofLogVerbose(__func__) << __LINE__ << " " << currentFrame << " of " << getTotalNumFrames();

                ENGINE_LOG("WE SHOULD LOOP");
                loopFrame = (int)((loop_offset*getFPS())/AV_TIME_BASE);
                sendOnLoop(__LINE__);
                
            }
        }
        if(!packet)
        {
            
            packet = omxReader.Read();
            if (packet && doLooping && packet->pts != DVD_NOPTS_VALUE)
            {
                packet->pts += loop_offset;
                packet->dts += loop_offset;
            }

        }
        
        bool isCacheEmpty = false;
        
        if (!packet)
        {
            if (hasAudio)
            {
                if (!audioPlayer->getCached() && !videoPlayer->getCached())
                {
                    isCacheEmpty = true;
                }
            }
            else
            {
                if (!videoPlayer->getCached())
                {
                    isCacheEmpty = true;
                }
            }
            
            if (omxReader.getIsEOF() && isCacheEmpty)
            {
                videoPlayer->submitEOS();
                
            }
            
            
            if(doLooping && omxReader.getIsEOF())
            {
                if (isCacheEmpty)
                {
                    if(omxReader.isStream)
                    {
                        if(!omxReader.fileObject)
                        {
                            doRestart = true; 
                        }else
                        {
                            seekToStart();
                        }
                        if (omxReader.wasFileRewound)
                        {
                            omxReader.wasFileRewound = false;
                        }
                        sendOnLoop(__LINE__);
                    }else
                    {
                        seekToStart();
                    }
                }
                else
                {
                    THREAD_SLEEP;
                    LOOP_LOG(" continue");
                    continue;
                }
                
            }
            else
            {
                if (!doLooping && omxReader.getIsEOF() && isCacheEmpty)
                {
                    if (videoPlayer->EOS())
                    {
                        onVideoEnd();
                        break;
                    }
                }
                
            }
            
            
        }
        
        
        if (hasAudio)
        {
            if(audioPlayer->getError())
            {
                ofLogError(__func__) << "audio player error.";
                hasAudio = false;
            }
        }

        if(packet)
        {
            LOOP_LOG("has packet");
            if(hasVideo && omxReader.isActive(OMXSTREAM_VIDEO, packet->stream_index))
            {
                if(videoPlayer->addPacket(packet))
                {
                    packet = NULL;
                }
                else
                {
                    THREAD_SLEEP;
                }
                
            }
            else if(hasAudio && packet->codec_type == AVMEDIA_TYPE_AUDIO)
            {
                if(audioPlayer->addPacket(packet))
                {
                    packet = NULL;
                }
                else
                {
                    THREAD_SLEEP;
                }
            }
            else
            {
                LOOP_LOG("freeing packet");
                omxReader.freePacket(packet, __func__);
                packet = NULL;
            }
        }else
        {
            LOOP_LOG("no packet, sleeping");
            THREAD_SLEEP;
        }

    }
}


#pragma mark playback commands

void ofxOMXPlayerEngine::setNormalSpeed()
{
    lock();
    speedMultiplier = 1;
    omxClock->setSpeed(normalPlaySpeed);
    omxReader.setSpeed(normalPlaySpeed);
    unlock();
}

int ofxOMXPlayerEngine::increaseSpeed()
{
    
    lock();
    doSeek = true;
    
    if(speedMultiplier+1 <=4)
    {
        speedMultiplier++;
        int newSpeed = normalPlaySpeed*speedMultiplier;
        
        omxClock->setSpeed(newSpeed);
        omxReader.setSpeed(newSpeed);
    }
    unlock();
    return speedMultiplier;
}

void ofxOMXPlayerEngine::rewind()
{
    if(speedMultiplier-1 == 0)
    {
        speedMultiplier = -1;
    }
    else
    {
        speedMultiplier--;
    }
    
    
    if(speedMultiplier<-8)
    {
        speedMultiplier = 1;
    }
    int newSpeed = normalPlaySpeed*speedMultiplier;
    
    omxClock->setSpeed(newSpeed);
    omxReader.setSpeed(newSpeed);
    
}

void ofxOMXPlayerEngine::scrubForward(int step)
{
    if (!isPaused())
    {
        setPaused(true);
    }
    if (step > 1) 
    {
        int count = step;
        while (count > 0) 
        {
            omxClock->step(1);
            count--;
        }
        setPaused(false);
    }else
    {
        omxClock->step(1);
        setPaused(false);
    }
    //enableAdjustments();
}

void ofxOMXPlayerEngine::stepFrameForward()
{
    stepFrame(1);
}

void ofxOMXPlayerEngine::stepFrame(int step)
{
    if (!isPaused())
    {
        setPaused(true);
    }
    if (step > 1) 
    {
        int count = step;
        while (count > 0) 
        {
            omxClock->step(1);
            count--;
        }
    }else
    {
        omxClock->step(1);
    }
}

void ofxOMXPlayerEngine::stop()
{
    setPaused(true);
}

bool ofxOMXPlayerEngine::setPaused(bool doPause)
{
    
    bool result = false;
    lock();
    if(doPause)
    {
        
        result =  omxClock->pause();
    }
    else
    {
        
        result =  omxClock->resume();
    }
    
    ofLogVerbose(__func__) << "result: " << result;
    unlock();
    return result;
}

void ofxOMXPlayerEngine::play()
{
    //ofLogVerbose(__func__) << "TODO: not sure what to do with this - reopen the player?";
}




#pragma mark getters

float ofxOMXPlayerEngine::getFPS()
{
    if (videoPlayer)
    {
        
        return videoPlayer->getFPS();
    }
    return 0;
}


float ofxOMXPlayerEngine::getDurationInSeconds()
{
    return duration;
}





void ofxOMXPlayerEngine::updateCurrentFrame()
{
    lock();
    if(directPlayer)
    {
        omxClock->getMediaTime();
    }
    frameCounter = omxClock->getFrameCounter();
    //ofLogVerbose(__func__) << "frameCounter: " << frameCounter;
    unlock();  
}
int ofxOMXPlayerEngine::getCurrentFrame()
{
    int result = 0;
    lock();
    result = frameCounter-loopFrame;
    unlock();
    return result;
}

int ofxOMXPlayerEngine::getTotalNumFrames()
{
    return nFrames;
}

int ofxOMXPlayerEngine::getWidth()
{
    return videoWidth;
}

int ofxOMXPlayerEngine::getHeight()
{
    return videoHeight;
}

double ofxOMXPlayerEngine::getMediaTime()
{
    double result = 0;
    lock();
    START();
    result =  omxClock->getMediaTime();
    END();
    P(2);
    unlock();
    return result;
}

bool ofxOMXPlayerEngine::isPaused()
{
    return omxClock->isPaused();
}


bool ofxOMXPlayerEngine::isPlaying()
{
    return bPlaying;
}

#pragma mark audio

void ofxOMXPlayerEngine::increaseVolume()
{
    if (!hasAudio || !didAudioOpen)
    {
        return;
    }
    
    float currentVolume = getVolume();
    currentVolume+=0.1;
    setVolume(currentVolume);
}


void ofxOMXPlayerEngine::decreaseVolume()
{
    if (!hasAudio || !didAudioOpen)
    {
        return;
    }
    
    float currentVolume = getVolume();
    currentVolume-=0.1;
    setVolume(currentVolume);
}

void ofxOMXPlayerEngine::setVolume(float volume)
{
    if (!hasAudio || !didAudioOpen)
    {
        return;
    }
    float value = ofMap(volume, 0.0, 1.0, -6000.0, 6000.0, true);
    audioPlayer->setCurrentVolume(value);
}

float ofxOMXPlayerEngine::getVolume()
{
    if (!hasAudio || !didAudioOpen || !audioPlayer)
    {
        return 0;
    }
    float value = ofMap(audioPlayer->getCurrentVolume(), -6000.0, 6000.0, 0.0, 1.0, true);
    return floorf(value * 100 + 0.5) / 100;
}

#pragma mark events

void ofxOMXPlayerEngine::addListener(ofxOMXPlayerListener* listener_)
{
    listener = listener_;
}

void ofxOMXPlayerEngine::removeListener()
{
    listener = NULL;
}



void ofxOMXPlayerEngine::onVideoEnd()
{
    if (listener != NULL)
    {
        
        ofxOMXPlayerListenerEventData eventData((void *)this);
        listener->onVideoEnd(eventData);
    }
    
}

#pragma mark shutdown

ofxOMXPlayerEngine::~ofxOMXPlayerEngine()
{
    //ofLogVerbose(__func__) << "omxReader.remainingPackets: " << omxReader.remainingPackets;

    if(isThreadRunning())
    {
        stopThread();
    }
    bPlaying = false;
    
    
    if (listener)
    {
        listener = NULL;
    }
    
    if (texturedPlayer)
    {
        delete texturedPlayer;
        texturedPlayer = NULL;
    }
    if (directPlayer)
    {
        delete directPlayer;
        directPlayer = NULL;
    }
    
    videoPlayer = NULL;
    
    
    if (audioPlayer)
    {
        delete audioPlayer;
        audioPlayer = NULL;
    }
    
    if(packet)
    {
        omxReader.freePacket(packet, __func__);
        packet = NULL;
    }
    
    omxReader.close();
    
    delete omxClock;
    omxClock = NULL;
    
}


