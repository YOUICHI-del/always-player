#pragma once
#include <QMutex>

// ★ v10: AlwaysPlayer.ini を書き換える処理（MainWindowのお気に入り・設定保存、
//   Playerの最後のフォルダ保存）は、すべてこのmutexで1本に直列化する。
//   別々のスレッドが同じiniを「読んで→書き換えて→置き換える」と、
//   後から書いた方が先の変更を上書きして消してしまうため。
inline QMutex &iniFileMutex()
{
    static QMutex m;
    return m;
}
