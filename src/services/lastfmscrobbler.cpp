#include "lastfmscrobbler.h"

LastFmScrobbler::LastFmScrobbler(Phonon::MediaObject *mediaObject)
{
    resetSongStatus();
    this->mediaObject = mediaObject;
    songsToScrobble = new QList<SongInfo>();
    connect(mediaObject, SIGNAL(stateChanged(Phonon::State,Phonon::State)), this, SLOT(handleStateChange(Phonon::State,Phonon::State)));

    // TODO: call this only if Last.fm is enabled.
    // Maybe do a function to call this, calling only
    // on applicaton start or when Last.fm is enabled.
    state = this->LastFmStateNone;
    netManager = new QNetworkAccessManager(this);
    handshake();
}


void LastFmScrobbler::handshake() {
    QDateTime time = QDateTime::currentDateTime();
    QString timeStamp = QString::number(time.toTime_t());

    QUrl url("http://post.audioscrobbler.com/");
    url.addQueryItem("hs", "true");
    url.addQueryItem("p", "1.2.1");
    url.addQueryItem("c", "adl");
    url.addQueryItem("v", "0.1");
    url.addQueryItem("u", LastFmSettings::username());
    url.addQueryItem("t", timeStamp);
    url.addQueryItem("a", generateToken(LastFmSettings::password(), timeStamp));

    state = LastFmStateWaitingToken;
    QNetworkRequest netRequest;
    netRequest.setUrl(url);
    qDebug("LastFmScrobbler: Asking to login to Last.fm...");
    authReply = netManager->get(netRequest);
    connect(authReply, SIGNAL(finished()), this, SLOT(readAuthenticationReply())); // TODO: handle error() signal

    // If we already have songs in the queue, scrobble them!
    if (songsToScrobble->count() > 0) {
        tryToScrobble();
    }

}

void LastFmScrobbler::readAuthenticationReply() {
    qDebug("LastFmScrobbler: Got reply!");
    QString replyString = authReply->readAll();
    if (state == LastFmStateWaitingToken) {
        QStringList lines = replyString.split('\n');
        qDebug("LastFmScrobbler: Last.fm answer: " + QString(lines.at(0)).toUtf8());
        if (lines.at(0) == "OK") {
            state = LastFmGotToken;
            sessionId = lines.at(1);
            nowPlayingUrl = lines.at(2);
            submissionUrl = lines.at(3);
            qDebug("LastFmScrobbler: Last.fm token: " + sessionId.toUtf8());
        }
        // TODO: better feedback for the user of what's wrong.
        // BANNED / BADAUTH / BADTIME / FAILED <reason>
        else {
            qDebug("LastFmScrobbler: Authentication problem! Disabling Last.fm");
            LastFmSettings::setActive(false);
            state = LastFmStateNone;
        }
    }
}

QString LastFmScrobbler::generateToken(QString input, QString timestamp) {
    /*
     * As said in http://www.last.fm/api/submissions#1.2 ,
     * we must create a token in the format
     * token = md5(md5(password) + timestamp)
     * to use in scrobble.
     */
    QByteArray encryptedPassword = QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Md5).toHex();
    QByteArray token = QCryptographicHash::hash(encryptedPassword + timestamp.toUtf8(), QCryptographicHash::Md5).toHex();
    return QString(token);
}

void LastFmScrobbler::onTick(qint64 time) {
    /*
     * If Last.fm was disabled, the signal will be disconnected only
     * when the Phonon state changes. This test avoid undesired
     * scrobbling.
     */
    if (!LastFmSettings::isActive()) return;

    /*
     * Since tickinterval = 1000 isn't asserting that onTick is called
     * every 1 second, we'll use lastTickTime to assert that we'll only
     * sum 1 to ellapsedTime when 1 second has passed.
     */
    if (time == lastTickTime) return;

    ellapsedTime++;
    lastTickTime = time;
    /*
     * We only can scrobble songs that were played for more than
     * 240s or half of its length.
     */
    if (ellapsedTime >= timeToScrobble || ellapsedTime >= 240) {
        songsToScrobble->append(currentSong);
        tryToScrobble();

        // Disconnect this slot if the song was already queued.
        disconnect(mediaObject, SIGNAL(tick(qint64)), this, SLOT(onTick(qint64)));
    }
}

void LastFmScrobbler::resetSongStatus() {
    ellapsedTime = 0;
    lastTickTime = 0;
    canScrobble = false;

    // Reset currentSong
    SongInfo resetSong;
    currentSong = resetSong;
}


void LastFmScrobbler::tryToScrobble() {
    qDebug("LastFmScrobbler: Trying to scrobble queued songs");
    // If we got no token or queue, nothing done.
    if (state != LastFmGotToken || songsToScrobble->count() == 0) return;

    // Try to scrobble queue
    QUrl url(submissionUrl);
    QString dataToPost = "s=" + sessionId;

    for (int i = 0; i < songsToScrobble->count(); i++) {
        SongInfo song = songsToScrobble->at(i);
        dataToPost += "&a[" + QString::number(i) + "]=" + song.artist + "&" +
                      "t[" + QString::number(i) + "]=" + song.title + "&" +
                      "b[" + QString::number(i) + "]=" + song.album + "&" +
                      "i[" + QString::number(i) + "]=" + song.startTimeStamp + "&" +
                      "l[" + QString::number(i) + "]=" + song.duration + "&" +
                      "n[" + QString::number(i) + "]=" + "&" + // TODO: track number
                      "m[" + QString::number(i) + "]=" + "&" + // TODO: MusicBrainz id
                      "r[" + QString::number(i) + "]=" + "&" + // TODO: rating
                      "o[" + QString::number(i) + "]=P";
    }
    //dataToPost = dataToPost.replace(' ',"%20");
    qDebug("LastFmScrobbler: Data to post to " + submissionUrl.toUtf8() + ": " + dataToPost.toUtf8());

    QNetworkRequest netRequest;
    netRequest.setUrl(url);
    qDebug("LastFmScrobbler: Scrobbling...");

    submissionReply = netManager->post(netRequest,dataToPost.toUtf8());
    connect(submissionReply, SIGNAL(readyRead()), this, SLOT(readSubmissionReply()));
}


void LastFmScrobbler::readSubmissionReply() {
    qDebug("LastFmScrobbler: Got submission reply!");
    QString replyString = QString::fromUtf8(submissionReply->readAll().replace('\n', ""));
    qDebug("LastFmScrobbler: Reply: " + QString(replyString).toAscii());

    // If we get an OK, clear our list of songs to scrobble.
    if (replyString == "OK") {
        songsToScrobble->clear();
    }
    // BADSESSION? Handshake again. And try to scrobble the songs after.
    else if (replyString == "BADSESSION") {
        handshake();
    }


}

void LastFmScrobbler::handleStateChange(Phonon::State newState, Phonon::State oldState) {
    // Disconnect slots if Last.fm is disabled.
    if (!LastFmSettings::isActive()) {
        qDebug("Last.fm is disabled");
        disconnect(mediaObject, SIGNAL(tick(qint64)), this, SLOT(onTick(qint64)));
        disconnect(mediaObject, SIGNAL(finished()), this, SLOT(resetSongStatus()));
        return;
    }


    // Save information to scrobble!
    if (newState == Phonon::PlayingState && (oldState == Phonon::StoppedState || oldState == Phonon::LoadingState)) {
        QMap<QString, QString> metaData = mediaObject->metaData();
        QString artist = metaData.value("ARTIST");
        QString album = metaData.value("ALBUM");
        QString title = metaData.value("TITLE");

        // We only can scrobble titles that have
        // artist and title defined.
        if (!artist.isEmpty() && !title.isEmpty() && mediaObject->totalTime() >= 30000) {
            timeToScrobble = qRound(mediaObject->totalTime() / 2000);
            connect(mediaObject, SIGNAL(tick(qint64)), this, SLOT(onTick(qint64)));
            connect(mediaObject, SIGNAL(finished()), this, SLOT(resetSongStatus()));

            canScrobble = true;

            // Reset currentSong
            SongInfo resetSong;
            currentSong = resetSong;
            currentSong.artist = artist;
            currentSong.title = title;
            currentSong.duration = QString::number(qRound(mediaObject->totalTime() / 1000));
            if (!album.isEmpty()) currentSong.album = album;

            QDateTime time = QDateTime::currentDateTime();
            currentSong.startTimeStamp = QString::number(time.toTime_t()).toUtf8();

            // Send a "Now Playing" notification to Last.fm
            QUrl url(nowPlayingUrl);
            QString dataToPost = "s=" + sessionId + "&" +
                                 "a=" + currentSong.artist + "&" +
                                 "t=" + currentSong.title + "&" +
                                 "b=" + currentSong.album + "&" +
                                 "l=" + currentSong.duration + "&" +
                                 "n=";

            QNetworkRequest netRequest;
            netRequest.setUrl(url);
            qDebug("Sending Now Playing...");
            nowPlayingReply = netManager->post(netRequest,dataToPost.toUtf8());
            connect(nowPlayingReply, SIGNAL(readyRead()), this, SLOT(readNowPlayingReply()));

        }
    }
    else if (newState == Phonon::StoppedState) {
        resetSongStatus();
    }

}

void LastFmScrobbler::readNowPlayingReply() {
    qDebug("Got Now Playing reply!");
    QString replyString = QString::fromUtf8(nowPlayingReply->readAll());
    qDebug("Reply: " + replyString.toUtf8());
}
