#include "AudioSystem.h"

// miniaudio 实现定义（单文件库，只能在一个 .cpp 中定义 MINIAUDIO_IMPLEMENTATION）。
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace {

std::string toLower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// Windows 上 std::filesystem::path::string() 用系统 ANSI 编码（GBK 等），中文路径会变问号。
// miniaudio 在 Windows 上用 CP_UTF8 转 widechar，故路径必须是 UTF-8。
// 这里用 wstring + WideCharToMultiByte(CP_UTF8) 转 UTF-8，保证目录扫描和显示都正确。
std::string wideToUtf8(const std::wstring& w) {
	if (w.empty()) {
		return {};
	}
	const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	if (len <= 0) {
		return {};
	}
	std::string out(static_cast<std::size_t>(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
	return out;
}

} // namespace

AudioSystem::AudioSystem() {
	_engine = new ma_engine();
	ma_engine_config config = ma_engine_config_init();
	config.noDevice = MA_FALSE; // 使用默认输出设备
	if (ma_engine_init(&config, _engine) != MA_SUCCESS) {
		delete _engine;
		_engine = nullptr;
	}
}

AudioSystem::~AudioSystem() {
	if (_sound) {
		ma_sound_stop(_sound);
		ma_sound_uninit(_sound);
		delete _sound;
		_sound = nullptr;
	}
	if (_engine) {
		ma_engine_uninit(_engine);
		delete _engine;
		_engine = nullptr;
	}
}

bool AudioSystem::scan(const std::string& directory) {
	_playlist.clear();
	_titles.clear();
	stop();
	_currentIndex = -1;

	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		const std::string ext = toLower(wideToUtf8(entry.path().extension().wstring()));
		if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".m4a") {
			// 路径存 wide（miniaudio wide API 用 _wfopen_s 打开，中文安全）；
			// 歌名转 UTF-8（imgui 显示）。
			_playlist.push_back(entry.path().wstring());
			_titles.push_back(wideToUtf8(entry.path().stem().wstring()));
		}
	}
	// 按文件名排序（目录顺序不确定）。
	std::vector<std::size_t> order(_titles.size());
	for (std::size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
		return toLower(_titles[a]) < toLower(_titles[b]);
	});
	std::vector<std::wstring> sortedPlaylist;
	std::vector<std::string> sortedTitles;
	sortedPlaylist.reserve(_playlist.size());
	sortedTitles.reserve(_titles.size());
	for (std::size_t i : order) {
		sortedPlaylist.push_back(_playlist[i]);
		sortedTitles.push_back(_titles[i]);
	}
	_playlist.swap(sortedPlaylist);
	_titles.swap(sortedTitles);

	return !_playlist.empty();
}

void AudioSystem::play(int index) {
	if (_engine == nullptr || _playlist.empty()) {
		return;
	}
	loadAndPlay(index);
}

void AudioSystem::pause() {
	if (!_sound || _paused) {
		return;
	}
	ma_uint64 cursor = 0;
	if (ma_sound_get_cursor_in_pcm_frames(_sound, &cursor) == MA_SUCCESS) {
		_soundCursor = static_cast<std::uint64_t>(cursor);
	}
	ma_sound_stop(_sound);
	_paused = true;
}

void AudioSystem::resume() {
	if (!_sound || !_paused) {
		return;
	}
	if (ma_sound_seek_to_pcm_frame(_sound, _soundCursor) != MA_SUCCESS) {
		ma_sound_seek_to_pcm_frame(_sound, 0);
	}
	ma_sound_start(_sound);
	_paused = false;
}

void AudioSystem::stop() {
	if (_sound) {
		ma_sound_stop(_sound);
		ma_sound_uninit(_sound);
		delete _sound;
		_sound = nullptr;
	}
	_paused = false;
	_soundCursor = 0;
	_currentIndex = -1;
}

void AudioSystem::next() {
	if (_playlist.empty()) {
		return;
	}
	const int idx = (_currentIndex < 0) ? 0 : (_currentIndex + 1) % static_cast<int>(_playlist.size());
	loadAndPlay(idx);
}

void AudioSystem::previous() {
	if (_playlist.empty()) {
		return;
	}
	const int idx = (_currentIndex <= 0) ? static_cast<int>(_playlist.size()) - 1 : _currentIndex - 1;
	loadAndPlay(idx);
}

void AudioSystem::setVolume(float volume) {
	_volume = std::clamp(volume, 0.0f, 1.0f);
	if (_sound) {
		ma_sound_set_volume(_sound, _volume);
	}
}

void AudioSystem::setLoopCurrent(bool loop) {
	_loopCurrent = loop;
	if (_sound) {
		ma_sound_set_looping(_sound, loop ? MA_TRUE : MA_FALSE);
	}
}

void AudioSystem::update() {
	if (!_autoAdvance.exchange(false)) {
		return;
	}
	if (_paused || !_sound) {
		return;
	}
	if (_loopCurrent) {
		// 单曲循环：回到开头（ma_sound_set_looping 正常情况不会走到这里，防御）。
		ma_sound_seek_to_pcm_frame(_sound, 0);
		ma_sound_start(_sound);
	} else {
		next();
	}
}

bool AudioSystem::isPlaying() const {
	return _sound != nullptr && !_paused && ma_sound_is_playing(_sound) == MA_TRUE;
}

const std::string& AudioSystem::currentTitle() const {
	static const std::string empty;
	if (_currentIndex >= 0 && _currentIndex < static_cast<int>(_titles.size())) {
		return _titles[_currentIndex];
	}
	return empty;
}

void AudioSystem::loadAndPlay(int index) {
	if (_engine == nullptr || _playlist.empty()) {
		return;
	}
	const int count = static_cast<int>(_playlist.size());
	index = (index % count + count) % count;

	stop();
	_currentIndex = -1;

	ma_sound* sound = new ma_sound();
	// 流式加载（长音乐省内存）；失败则回退非流式。
	// 用 wide 版本（ma_sound_init_from_file 内部 fopen_s 走 ANSI 码页，中文路径会失败）。
	ma_result r = ma_sound_init_from_file_w(_engine, _playlist[index].c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, sound);
	if (r != MA_SUCCESS) {
		ma_sound_uninit(sound);
		delete sound;
		sound = new ma_sound();
		r = ma_sound_init_from_file_w(_engine, _playlist[index].c_str(), 0, nullptr, nullptr, sound);
		if (r != MA_SUCCESS) {
			ma_sound_uninit(sound);
			delete sound;
			return;
		}
	}

	_sound = sound;
	_currentIndex = index;
	_paused = false;
	_soundCursor = 0;
	ma_sound_set_volume(_sound, _volume);
	ma_sound_set_looping(_sound, _loopCurrent ? MA_TRUE : MA_FALSE);
	ma_sound_set_end_callback(_sound, &AudioSystem::soundEndCallback, this);
	ma_sound_start(_sound);
}

void AudioSystem::onSoundEnded() {
	// 音频线程回调：只置标志，主线程 update() 处理切歌。
	_autoAdvance = true;
}

void AudioSystem::soundEndCallback(void* userData, ma_sound* /*sound*/) {
	auto* self = static_cast<AudioSystem*>(userData);
	if (self) {
		self->onSoundEnded();
	}
}
