#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct ma_engine;
struct ma_sound;

// 轻量音乐播放器（miniaudio 单文件库，WAV/MP3/FLAC/OGG 流式播放）。
// 参考项目无音频系统，本类为新增功能：播放/暂停/上一首/下一首/音量/单曲循环/自动切歌。
// 线程安全：音频线程回调（ma_sound end callback）只置原子标志，主线程每帧 update() 处理切歌。
class AudioSystem {
public:
	AudioSystem();
	~AudioSystem();

	AudioSystem(const AudioSystem&) = delete;
	AudioSystem& operator=(const AudioSystem&) = delete;

	// 扫描目录中的音乐文件（mp3/wav/flac/ogg/m4a，按文件名排序）。
	bool scan(const std::string& directory);

	// 播放指定曲目（越界自动回绕）。
	void play(int index);
	// 暂停（保留播放位置）。
	void pause();
	// 恢复播放。
	void resume();
	// 停止并释放当前曲目。
	void stop();
	void next();
	void previous();

	void setVolume(float volume);
	void setLoopCurrent(bool loop);

	// 每帧调用：处理曲目自然结束后的自动切歌。
	void update();

	bool isPlaying() const;
	bool isPaused() const { return _paused; }
	bool loopCurrent() const { return _loopCurrent; }
	float volume() const { return _volume; }
	int currentIndex() const { return _currentIndex; }
	bool hasMusic() const { return !_playlist.empty(); }
	const std::vector<std::string>& titles() const { return _titles; }
	const std::string& currentTitle() const;

private:
	void loadAndPlay(int index);
	void onSoundEnded();
	static void soundEndCallback(void* userData, ma_sound* sound);

	ma_engine* _engine = nullptr;
	ma_sound* _sound = nullptr;
	// 完整路径用 wide（miniaudio 的 ma_sound_init_from_file_w 用 _wfopen_s 正确处理中文路径）；
	// 歌名用 UTF-8（imgui 显示，合并了系统中文字体）。
	std::vector<std::wstring> _playlist;
	std::vector<std::string> _titles;   // 文件名（去扩展名，UTF-8）
	int _currentIndex = -1;
	float _volume = 0.8f;
	bool _paused = false;
	bool _loopCurrent = false;
	std::uint64_t _soundCursor = 0;
	std::atomic<bool> _autoAdvance{ false };
};
