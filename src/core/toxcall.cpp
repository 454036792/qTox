#include "src/core/toxcall.h"
#include "src/audio/audio.h"
#include "src/core/coreav.h"
#include "src/persistence/settings.h"
#include "src/video/camerasource.h"
#include "src/video/corevideosource.h"
#include <QTimer>
#include <QtConcurrent/QtConcurrent>

/**
 * @var uint32_t ToxCall::callId
 * @brief Could be a friendNum or groupNum, must uniquely identify the call. Do not modify!
 *
 * @var bool ToxCall::inactive
 * @brief True while we're not participating. (stopped group call, ringing but hasn't started yet,
 * ...)
 *
 * @var bool ToxFriendCall::videoEnabled
 * @brief True if our user asked for a video call, sending and recieving.
 *
 * @var bool ToxFriendCall::nullVideoBitrate
 * @brief True if our video bitrate is zero, i.e. if the device is closed.
 *
 * @var TOXAV_FRIEND_CALL_STATE ToxFriendCall::state
 * @brief State of the peer (not ours!)
 *
 * @var QMap ToxGroupCall::peers
 * @brief Keeps sources for users in group calls.
 */

ToxCall::ToxCall(bool VideoEnabled, CoreAV& av)
    : av{&av}
    , videoEnabled{VideoEnabled}
{
}

ToxCall::~ToxCall()
{
    Audio& audio = Audio::getInstance();

    QObject::disconnect(audioInConn);
    audio.unsubscribeInput();
    if (videoEnabled) {
        QObject::disconnect(videoInConn);
        CameraSource::getInstance().unsubscribe();
    }
}

bool ToxCall::isActive() const
{
    return active;
}

void ToxCall::setActive(bool value)
{
    active = value;
}

bool ToxCall::getMuteVol() const
{
    return muteVol;
}

void ToxCall::setMuteVol(bool value)
{
    muteVol = value;
}

bool ToxCall::getMuteMic() const
{
    return muteMic;
}

void ToxCall::setMuteMic(bool value)
{
    muteMic = value;
}

bool ToxCall::getVideoEnabled() const
{
    return videoEnabled;
}

void ToxCall::setVideoEnabled(bool value)
{
    videoEnabled = value;
}

bool ToxCall::getNullVideoBitrate() const
{
    return nullVideoBitrate;
}

void ToxCall::setNullVideoBitrate(bool value)
{
    nullVideoBitrate = value;
}

CoreVideoSource* ToxCall::getVideoSource() const
{
    return videoSource;
}

quint32 ToxFriendCall::getAlSource() const
{
    return alSource;
}

void ToxFriendCall::setAlSource(const quint32& value)
{
    alSource = value;
}

ToxFriendCall::ToxFriendCall(uint32_t FriendNum, bool VideoEnabled, CoreAV& av)
    : ToxCall(VideoEnabled, av)
{
    // register audio
    Audio& audio = Audio::getInstance();
    audio.subscribeInput();
    audioInConn = QObject::connect(&Audio::getInstance(), &Audio::frameAvailable,
    [&av, FriendNum](const int16_t* pcm, size_t samples, uint8_t chans,
            uint32_t rate) {
        av.sendCallAudio(FriendNum, pcm, samples, chans, rate);
    });

    if (!audioInConn) {
       qDebug() << "Audio input connection not working";
    }

    audio.subscribeOutput(alSource);

    // register video
    if (videoEnabled) {
        videoSource = new CoreVideoSource();
        CameraSource& source = CameraSource::getInstance();

        if (source.isNone()) {
            source.setupDefault();
        }
        source.subscribe();
        videoInConn = QObject::connect(&source, &VideoSource::frameAvailable,
                                       [&av, FriendNum](std::shared_ptr<VideoFrame> frame) {
                                           av.sendCallVideo(FriendNum, frame);
                                       });
        if (!videoInConn) {
            qDebug() << "Video connection not working";
        }
    }
}

ToxFriendCall::~ToxFriendCall()
{
    auto& audio = Audio::getInstance();
    audio.unsubscribeOutput(alSource);
}

void ToxFriendCall::startTimeout(uint32_t callId)
{
    if (!timeoutTimer) {
        timeoutTimer = std::unique_ptr<QTimer>{new QTimer{}};
        // We might move, so we need copies of members. CoreAV won't move while we're alive
        CoreAV* avCopy = av;
        QObject::connect(timeoutTimer.get(), &QTimer::timeout,
                         [avCopy, callId]() { avCopy->timeoutCall(callId); });
    }

    if (!timeoutTimer->isActive())
        timeoutTimer->start(CALL_TIMEOUT);
}

void ToxFriendCall::stopTimeout()
{
    if (!timeoutTimer)
        return;

    timeoutTimer->stop();
    timeoutTimer.reset();
}

TOXAV_FRIEND_CALL_STATE ToxFriendCall::getState() const
{
    return state;
}

void ToxFriendCall::setState(const TOXAV_FRIEND_CALL_STATE& value)
{
    state = value;
}

ToxGroupCall::ToxGroupCall(int GroupNum, CoreAV& av)
    : ToxCall(false, av)
{
    // register audio
    Audio& audio = Audio::getInstance();
    audio.subscribeInput();
    audioInConn = QObject::connect(&Audio::getInstance(), &Audio::frameAvailable,
    [&av, GroupNum](const int16_t* pcm, size_t samples, uint8_t chans,
            uint32_t rate) {
        av.sendGroupCallAudio(GroupNum, pcm, samples, chans, rate);
    });

    if (!audioInConn) {
       qDebug() << "Audio input connection not working";
    }
}

ToxGroupCall::~ToxGroupCall()
{
    Audio& audio = Audio::getInstance();

    for (quint32 v : peers) {
        audio.unsubscribeOutput(v);
    }
}

void ToxGroupCall::removePeer(ToxPk peerId)
{
    auto& audio = Audio::getInstance();
    const auto& sourceId = peers.find(peerId);
    if(sourceId == peers.constEnd()) {
        qDebug() << "Peer does not have a source, can't remove";
        return;
    }

    audio.unsubscribeOutput(sourceId.value());
    peers.remove(peerId);
}

void ToxGroupCall::addPeer(ToxPk peerId)
{
    auto& audio = Audio::getInstance();
    uint sourceId = 0;
    audio.subscribeOutput(sourceId);
    peers.insert(peerId, sourceId);
}

bool ToxGroupCall::havePeer(ToxPk peerId)
{
    const auto& sourceId = peers.find(peerId);
    return sourceId != peers.constEnd();
}

void ToxGroupCall::clearPeers()
{
    Audio& audio = Audio::getInstance();

    for (quint32 v : peers) {
        audio.unsubscribeOutput(v);
    }

    peers.clear();
}

quint32 ToxGroupCall::getAlSource(ToxPk peer)
{
    if(!havePeer(peer)) {
        qWarning() << "Getting alSource for non-existant peer";
        return 0;
    }

    return peers[peer];
}
