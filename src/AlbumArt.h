#pragma once
// ★ v10: アルバムアート（ジャケット画像）を探す処理をひとつにまとめたもの。
//   再生中の曲（Player::getCoverArt）とアルバムブラウザ・フォルダ表示（MainWindow::findAlbumArt）が
//   同じ順番で探すようにする。
//
//   探す順番：
//     1. 曲ファイルに埋め込まれた画像（MP3・WAV・AIFF・FLAC・M4A・OGG・Opus・DSF・DFF）
//     2. 同じフォルダの画像ファイル（cover.jpg → folder.jpg → front.jpg → … → 最初に見つかった画像）
//     3. インターネットから取得して保存しておいた画像（アプリのキャッシュ。曲ファイルには書き込まない）
//   3 の取得（fetchOnline）は、1・2 が無いときだけ、設定でオンのときだけ行う。
#include <QString>
#include <QByteArray>

namespace AlbumArt {

struct Tags {
    QString artist;   // アルバムアーティスト（無ければアーティスト）
    QString album;
};

// 1. 曲ファイルに埋め込まれた画像のデータ（無ければ空）。表紙（Front Cover）を優先する。
QByteArray embedded(const QString &audioFile);

// 2. フォルダの画像ファイルのパス（無ければ空）
QString folderImage(const QString &dir);

// フォルダ（サブフォルダは2階層まで）の最初の音楽ファイル
QString firstAudioIn(const QString &dir);

// アーティスト名・アルバム名（ネット取得とキャッシュの名前に使う）
Tags readTags(const QString &audioFile);

// 3. キャッシュ済みの画像のパス（無ければ空）
QString cachedFor(const Tags &t);
QString cachedFor(const QString &audioFile);

// キャッシュへ保存し、そのパスを返す（失敗したら空）
QString saveToCache(const Tags &t, const QByteArray &image);

// ネットから取得する（時間がかかるので、別スレッドで呼ぶこと）。見つからなければ空。
//   iTunes → MusicBrainz / Cover Art Archive の順。アルバム名とアーティスト名が
//   一致したものだけを使う（似た名前の別のアルバムの画像を出さないため）。
QByteArray fetchOnline(const Tags &t);

// 画像データの拡張子（".png" / ".jpg" など）
QString extensionFor(const QByteArray &image);

// 設定：ネットから取得するか（既定はオン）
bool onlineEnabled();
void setOnlineEnabled(bool on);

} // namespace AlbumArt
