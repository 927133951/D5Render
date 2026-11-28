#pragma once
#include <string>
#include <array>
#include <cstddef>
#include <cstdint>
namespace obf {
template<uint32_t Key, std::size_t N>
struct ObfString {
	std::array<char, N> data{};
	constexpr ObfString(const char(&str)[N]) : data{} {
		for (std::size_t i = 0; i < N; ++i) {
			data[i] = str[i] ^ static_cast<char>((Key >> ((i % 4) * 8)) & 0xFF);
		}
	}
	constexpr std::size_t size() const { return N - 1; }
	inline std::string decode() const {
		std::string result(N - 1, '\0');
		for (std::size_t i = 0; i < N - 1; ++i) {
			result[i] = data[i] ^ static_cast<char>((Key >> ((i % 4) * 8)) & 0xFF);
		}
		return result;
	}
	inline std::wstring decodeW() const {
		std::string narrow = decode();
		std::wstring result(narrow.size(), L'\0');
		for (std::size_t i = 0; i < narrow.size(); ++i) {
			result[i] = static_cast<wchar_t>(narrow[i]);
		}
		return result;
	}
};
template<uint32_t Key, std::size_t N>
struct ObfWString {
	std::array<wchar_t, N> data{};
	constexpr ObfWString(const wchar_t(&str)[N]) : data{} {
		for (std::size_t i = 0; i < N; ++i) {
			uint32_t k = Key;
			k = ((k << 5) + k) ^ static_cast<uint32_t>(i * 0x9E3779B9);
			data[i] = str[i] ^ static_cast<wchar_t>(k & 0xFFFF);
		}
	}
	inline std::wstring decode() const {
		std::wstring result(N - 1, L'\0');
		for (std::size_t i = 0; i < N - 1; ++i) {
			uint32_t k = Key;
			k = ((k << 5) + k) ^ static_cast<uint32_t>(i * 0x9E3779B9);
			result[i] = data[i] ^ static_cast<wchar_t>(k & 0xFFFF);
		}
		return result;
	}
};
struct SecureString {
	std::string data;
	SecureString() = default;
	SecureString(const std::string& s) : data(s) {}
	SecureString(std::string&& s) noexcept : data(std::move(s)) {}
	~SecureString() { secureClear(); }
	SecureString(const SecureString&) = default;
	SecureString& operator=(const SecureString&) = default;
	SecureString& operator=(SecureString&& o) noexcept {
		if (this != &o) { secureClear(); data = std::move(o.data); }
		return *this;
	}
	void secureClear() {
		if (!data.empty()) {
			volatile char* p = &data[0];
			for (size_t i = 0; i < data.size(); ++i) p[i] = 0;
			data.clear();
		}
	}
	const char* c_str() const { return data.c_str(); }
	operator std::string() const { return data; }
};
}
#define OBF_KEY_1 0x5A3C7E1Du
#define OBF_KEY_2 0x4B2D6F0Au
#define OBF_KEY_3 0x7E1F3A5Cu
#define OBF_KEY_4 0x1D4E8B2Fu
#define OBF_KEY_5 0x6C9A0D3Eu
#define OBF_KEY_6 0x3F7B2E1Au
#define OBF_KEY_7 0x8D5C4A27u
#define OBF_KEY_8 0x2E6F9B13u
#define OBS1(s) ([]() -> std::string { static constexpr auto _e = obf::ObfString<OBF_KEY_1, sizeof(s)>(s); return _e.decode(); }())
#define OBS2(s) ([]() -> std::string { static constexpr auto _e = obf::ObfString<OBF_KEY_2, sizeof(s)>(s); return _e.decode(); }())
#define OBS3(s) ([]() -> std::string { static constexpr auto _e = obf::ObfString<OBF_KEY_3, sizeof(s)>(s); return _e.decode(); }())
#define OBS4(s) ([]() -> std::string { static constexpr auto _e = obf::ObfString<OBF_KEY_4, sizeof(s)>(s); return _e.decode(); }())
#define OBS5(s) ([]() -> std::string { static constexpr auto _e = obf::ObfString<OBF_KEY_5, sizeof(s)>(s); return _e.decode(); }())
#define OBS6(s) ([]() -> std::string { static constexpr auto _e = obf::ObfString<OBF_KEY_6, sizeof(s)>(s); return _e.decode(); }())
#define OBS7(s) ([]() -> std::string { static constexpr auto _e = obf::ObfString<OBF_KEY_7, sizeof(s)>(s); return _e.decode(); }())
#define OBS8(s) ([]() -> std::string { static constexpr auto _e = obf::ObfString<OBF_KEY_8, sizeof(s)>(s); return _e.decode(); }())
#define OBSWS1(s) ([]() -> std::wstring { static constexpr auto _e = obf::ObfWString<OBF_KEY_1, sizeof(s)/sizeof(wchar_t)>(s); return _e.decode(); }())
#define OBSWS2(s) ([]() -> std::wstring { static constexpr auto _e = obf::ObfWString<OBF_KEY_2, sizeof(s)/sizeof(wchar_t)>(s); return _e.decode(); }())
#define OBSWS3(s) ([]() -> std::wstring { static constexpr auto _e = obf::ObfWString<OBF_KEY_3, sizeof(s)/sizeof(wchar_t)>(s); return _e.decode(); }())
#define OBSWS4(s) ([]() -> std::wstring { static constexpr auto _e = obf::ObfWString<OBF_KEY_4, sizeof(s)/sizeof(wchar_t)>(s); return _e.decode(); }())
#define OBSWS5(s) ([]() -> std::wstring { static constexpr auto _e = obf::ObfWString<OBF_KEY_5, sizeof(s)/sizeof(wchar_t)>(s); return _e.decode(); }())
#define OBSWS6(s) ([]() -> std::wstring { static constexpr auto _e = obf::ObfWString<OBF_KEY_6, sizeof(s)/sizeof(wchar_t)>(s); return _e.decode(); }())
#define OBSWS7(s) ([]() -> std::wstring { static constexpr auto _e = obf::ObfWString<OBF_KEY_7, sizeof(s)/sizeof(wchar_t)>(s); return _e.decode(); }())
#define OBSWS8(s) ([]() -> std::wstring { static constexpr auto _e = obf::ObfWString<OBF_KEY_8, sizeof(s)/sizeof(wchar_t)>(s); return _e.decode(); }())
#define OBS(s) OBS1(s)
#define OBSS(s) obf::SecureString(OBS(s))
#define OBSW(s) OBSWS1(s)
