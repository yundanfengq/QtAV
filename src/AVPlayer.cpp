/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2012-2013 Wang Bin <wbsecg1@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <QtAV/AVPlayer.h>

#include <qevent.h>
#include <qpainter.h>
#include <QApplication>
#include <QEvent>
#include <QtCore/QDir>

#include <QtAV/AVDemuxer.h>
#include <QtAV/AudioThread.h>
#include <QtAV/Packet.h>
#include <QtAV/AudioDecoder.h>
#include <QtAV/VideoRenderer.h>
#include <QtAV/AVClock.h>
#include <QtAV/VideoCapture.h>
#include <QtAV/VideoDecoder.h>
#include <QtAV/WidgetRenderer.h>
#include <QtAV/VideoThread.h>
#include <QtAV/AVDemuxThread.h>
#include <QtAV/EventFilter.h>
#include <QtAV/VideoCapture.h>
#if HAVE_OPENAL
#include <QtAV/AOOpenAL.h>
#endif //HAVE_OPENAL
#if HAVE_PORTAUDIO
#include <QtAV/AOPortAudio.h>
#endif //HAVE_PORTAUDIO
#ifdef __cplusplus
extern "C"
{
#endif //__cplusplus
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif //__cplusplus

namespace QtAV {

AVPlayer::AVPlayer(QObject *parent) :
    QObject(parent),loaded(false),capture_dir("capture"),renderer(0),audio(0)
  ,event_filter(0),video_capture(0)
{
    qDebug("QtAV %s\nCopyright (C) 2012 Wang Bin <wbsecg1@gmail.com>"
           "\nDistributed under GPLv3 or later"
           "\nShanghai University, China"
           , QTAV_VERSION_STR_LONG);
    /*
     * call stop() before the window(renderer) closed to stop the waitcondition
     * If close the renderer widget, the the renderer may destroy before waking up.
     */
    connect(qApp, SIGNAL(aboutToQuit()), SLOT(stop()));
    avTimerId = -1;
    clock = new AVClock(AVClock::AudioClock);
    //clock->setClockType(AVClock::ExternalClock);
	demuxer.setClock(clock);
    connect(&demuxer, SIGNAL(started()), clock, SLOT(start()));
#if HAVE_OPENAL
    audio = new AOOpenAL();
#elif HAVE_PORTAUDIO
    audio = new AOPortAudio();
#endif
    audio_dec = new AudioDecoder();
    audio_thread = new AudioThread(this);
    audio_thread->setClock(clock);
    //audio_thread->setPacketQueue(&audio_queue);
    audio_thread->setDecoder(audio_dec);
    audio_thread->setOutput(audio);

    video_dec = new VideoDecoder();

    video_thread = new VideoThread(this);
    video_thread->setClock(clock);
    video_thread->setDecoder(video_dec);

    demuxer_thread = new AVDemuxThread(this);
    demuxer_thread->setDemuxer(&demuxer);
    demuxer_thread->setAudioThread(audio_thread);
    demuxer_thread->setVideoThread(video_thread);

    event_filter = new EventFilter(this);

    setVideoCapture(new VideoCapture());
}

AVPlayer::~AVPlayer()
{
    if (avTimerId > 0)
        killTimer(avTimerId);
    stop();
    if (audio) {
        delete audio;
        audio = 0;
    }
    if (audio_dec) {
        delete audio_dec;
        audio_dec = 0;
    }
    if (video_dec) {
        delete video_dec;
        video_dec = 0;
    }
}

AVClock* AVPlayer::masterClock()
{
    return clock;
}

void AVPlayer::setRenderer(VideoRenderer *r)
{
    if (renderer) {
		if (isPlaying())
			stop();
		//delete renderer; //Do not own the ptr
    }
    renderer = r;
    renderer->registerEventFilter(event_filter);
    video_thread->setOutput(renderer);
    renderer->resizeVideo(renderer->videoSize()); //IMPORTANT: the swscaler will resize
}

void AVPlayer::setMute(bool mute)
{
    if (audio)
        audio->setMute(mute);
}

bool AVPlayer::isMute() const
{
    return !audio || audio->isMute();
}

//TODO: remove?
void AVPlayer::resizeVideo(const QSize &size)
{
    renderer->resizeVideo(size); //TODO: deprecate
    //video_dec->resizeVideo(size);
}
/*
 * loaded state is the state of current setted file.
 * For replaying, we can avoid load a seekable file again.
 * For playing a new file, load() is required.
 */
void AVPlayer::setFile(const QString &path)
{
    this->path = path;
    loaded = false; //
    //qApp->activeWindow()->setWindowTitle(path); //crash on linux
}

QString AVPlayer::file() const
{
    return path;
}

VideoCapture* AVPlayer::setVideoCapture(VideoCapture *cap)
{
    VideoCapture *old = video_capture;
    video_capture = cap;
    video_thread->setVideoCapture(video_capture);
    return old;
}

VideoCapture* AVPlayer::videoCapture()
{
    return video_capture;
}

void AVPlayer::setCaptureName(const QString &name)
{
    capture_name = name;
}

void AVPlayer::setCaptureSaveDir(const QString &dir)
{
    capture_dir = dir;
}

bool AVPlayer::captureVideo()
{
    if (!video_capture)
        return false;
    bool pause_old = isPaused();
    if (!video_capture->isAsync())
        pause(true);
    video_capture->setCaptureDir(capture_dir);
    QString cap_name(capture_name);
    if (cap_name.isEmpty())
        cap_name = QFileInfo(path).completeBaseName();
    //FIXME: pts is not correct because of multi-thread
    double pts = video_thread->currentPts();
    cap_name += "_" + QString::number(pts, 'f', 3);
    video_capture->setCaptureName(cap_name);
    qDebug("request capture: %s", qPrintable(cap_name));
    video_capture->request();
    if (!video_capture->isAsync())
        pause(pause_old);
    return true;
}

bool AVPlayer::play(const QString& path)
{
    setFile(path);
    play();
    return true;//isPlaying();
}

bool AVPlayer::isPlaying() const
{
	return demuxer_thread->isRunning() || audio_thread->isRunning() || video_thread->isRunning();
}

void AVPlayer::pause(bool p)
{
    //pause thread. check pause state?
    demuxer_thread->pause(p);
    audio_thread->pause(p);
    video_thread->pause(p);
    clock->pause(p);
#if 0
    /*Pause output. all threads using those outputs will be paused. If a output is not paused
     *, then other players' avthread can use it.
     */
    if (audio)
        audio->pause(p);
    if (renderer)
        renderer->pause(p);
#endif
}

bool AVPlayer::isPaused() const
{
    return demuxer_thread->isPaused() | audio_thread->isPaused() | video_thread->isPaused();
#if 0
    bool p = false;
    if (audio)
        p |= audio->isPaused();
    if (renderer)
        p |= renderer->isPaused();
    return p;
#endif
}

bool AVPlayer::isLoaded() const
{
    return loaded;
}

bool AVPlayer::load(const QString &path)
{
    setFile(path);
    return load();
}

bool AVPlayer::load()
{
    loaded = false;
    if (path.isEmpty()) {
        qDebug("No file to play...");
        return loaded;
    }
    qDebug("loading: %s ...", path.toUtf8().constData());
    if (!demuxer.loadFile(path)) {
        return loaded;
    }
    loaded = true;
    demuxer.dump();
    formatCtx = demuxer.formatContext();
    aCodecCtx = demuxer.audioCodecContext();
    vCodecCtx = demuxer.videoCodecContext();
    if (audio && aCodecCtx) {
        audio->setSampleRate(aCodecCtx->sample_rate);
        audio->setChannels(aCodecCtx->channels);
        if (!audio->open()) {
            //return; //audio not ready
        }
    }
    audio_dec->setCodecContext(aCodecCtx);
    video_dec->setCodecContext(vCodecCtx);
    return loaded;
}

//FIXME: why no demuxer will not get an eof if replaying by seek(0)?
void AVPlayer::play()
{
    if (isPlaying())
        stop();
    /*
     * avoid load mutiple times when replaying the same seekable file
     * TODO: force load unseekable stream? avio.seekable. currently you
     * must setFile() agian to reload an unseekable stream
     */
    if (!isLoaded()) { //if (!isLoaded() && !load())
        if (!load())
            return;
    } else {
        demuxer.seek(0); //FIXME: now assume it is seekable. for unseekable, setFile() again
    }
    Q_ASSERT(clock != 0);
    clock->reset();

    if (aCodecCtx) {
        qDebug("Starting audio thread...");
        audio_thread->start(QThread::HighestPriority);
    }
    if (vCodecCtx) {
        qDebug("Starting video thread...");
        video_thread->start();
    }
    demuxer_thread->start();
    emit started();
}

void AVPlayer::stop()
{
    if (demuxer_thread->isRunning()) {
        qDebug("stop d");
        demuxer_thread->stop();
        //wait for finish then we can safely set the vars, e.g. a/v decoders
        if (!demuxer_thread->wait()) {
            qWarning("Timeout waiting for demux thread stopped. Terminate it.");
            demuxer_thread->terminate(); //Terminate() causes the wait condition destroyed without waking up
        }
    }
    if (audio_thread->isRunning()) {
        qDebug("stop a");
        audio_thread->stop();
        if (!audio_thread->wait(1000)) {
            qWarning("Timeout waiting for audio thread stopped. Terminate it.");
            audio_thread->terminate();
        }
    }
    if (video_thread->isRunning()) {
        qDebug("stopv");
        video_thread->stop();
        if (!video_thread->wait(1000)) {
            qWarning("Timeout waiting for video thread stopped. Terminate it.");
            video_thread->terminate(); ///if time out
        }
    }
    emit stopped();
}
//FIXME: If not playing, it will just play but not play one frame.
void AVPlayer::playNextFrame()
{
    if (!isPlaying()) {
        play();
    }
    pause(false);
    pause(true);
}

void AVPlayer::seek(qreal pos)
{
    demuxer_thread->seek(pos);
}

void AVPlayer::seekForward()
{
    demuxer_thread->seekForward();
}

void AVPlayer::seekBackward()
{
    demuxer_thread->seekBackward();
}

void AVPlayer::updateClock(qint64 msecs)
{
    clock->updateExternalClock(msecs);
}

//TODO: what if no audio stream?
void AVPlayer::timerEvent(QTimerEvent* e)
{
    if (e->timerId() != avTimerId)
        return;
    AVPacket packet;
    int videoStream = demuxer.videoStream();
    int audioStream = demuxer.audioStream();
    while (av_read_frame(formatCtx, &packet) >=0 ) {
        Packet pkt;
        pkt.data = QByteArray((const char*)packet.data, packet.size);
        pkt.duration = packet.duration;
        if (packet.dts != AV_NOPTS_VALUE) //has B-frames
            pkt.pts = packet.dts;
        else if (packet.pts != AV_NOPTS_VALUE)
            pkt.pts = packet.pts;
        else
            pkt.pts = 0;
        AVStream *stream = formatCtx->streams[packet.stream_index];
        pkt.pts *= av_q2d(stream->time_base);

        if (stream->codec->codec_type == AVMEDIA_TYPE_SUBTITLE
                && (packet.flags & AV_PKT_FLAG_KEY)
                &&  packet.convergence_duration != AV_NOPTS_VALUE)
            pkt.duration = packet.convergence_duration * av_q2d(stream->time_base);
        else if (packet.duration > 0)
            pkt.duration = packet.duration * av_q2d(stream->time_base);
        else
            pkt.duration = 0;

        if (packet.stream_index == audioStream) {
            audio_thread->packetQueue()->put(pkt);
            av_free_packet(&packet); //TODO: why is needed for static var?
        } else if (packet.stream_index == videoStream) {
            if (video_dec->decode(QByteArray((char*)packet.data, packet.size)))
                renderer->writeData(video_dec->data());
            break;
        } else { //subtitle
            av_free_packet(&packet);
            continue;
        }
    }
}

} //namespace QtAV
