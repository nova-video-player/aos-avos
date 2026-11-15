// Copyright 2026 Courville Software
// SPDX-License-Identifier: Apache-2.0
#include "sub_language.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef CONFIG_FASTTEXT
#include <fasttext/fasttext.h>
#endif

namespace {
const size_t MAX_TEXT = 4096, MIN_TEXT = 512, MAX_PACKET = 16384;
const unsigned MAX_TRACKS = 128, MIN_CUES = 8;
std::mutex pathMutex, metadataMutex;
std::string modelPath;
std::atomic<int> loadedModels(0);

bool asciiAlpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool word(unsigned char c) { return asciiAlpha(c) || c >= 128 || (c >= '0' && c <= '9'); }
std::string lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
bool usable(const std::string &s) {
    const auto end = s.find('-');
    const std::string base = s.substr(0, end);
    if (base.size() < 2 || base.size() > 3 || base == "und" || base == "mul" || base == "zxx"
            || base == "unk") return false;
    if (!std::all_of(base.begin(), base.end(), asciiAlpha)) return false;
    if (end == std::string::npos) return true;
    size_t start = end + 1;
    while (start < s.size()) {
        size_t stop = s.find('-', start);
        if (stop == std::string::npos) stop = s.size();
        if (stop == start || stop - start > 8) return false;
        for (size_t i = start; i < stop; ++i)
            if (!asciiAlpha(s[i]) && !(s[i] >= '0' && s[i] <= '9')) return false;
        if (stop == s.size()) return true;
        start = stop + 1;
    }
    return false;
}
struct Language { const char *code; const char *iso3; const char *name; const char *alias; };
// Explicit names and ISO aliases, never guesses from generic track descriptions.
const Language languages[] = {
    {"en","eng","english","anglais"}, {"fr","fra","french","français"},
    {"de","deu","german","deutsch"}, {"es","spa","spanish","español"},
    {"pt","por","portuguese","português"}, {"it","ita","italian","italiano"},
    {"nl","nld","dutch","nederlands"}, {"da","dan","danish","dansk"},
    {"no","nor","norwegian","norsk"}, {"sv","swe","swedish","svenska"},
    {"fi","fin","finnish","suomi"}, {"is","isl","icelandic","íslenska"},
    {"ar","ara","arabic","العربية"}, {"he","heb","hebrew","עברית"},
    {"el","ell","greek","ελληνικά"}, {"cs","ces","czech","čeština"},
    {"sk","slk","slovak","slovenčina"}, {"pl","pol","polish","polski"},
    {"hu","hun","hungarian","magyar"}, {"ro","ron","romanian","română"},
    {"ru","rus","russian","русский"}, {"uk","ukr","ukrainian","українська"},
    {"bg","bul","bulgarian","български"}, {"hr","hrv","croatian","hrvatski"},
    {"sr","srp","serbian","српски"}, {"sl","slv","slovenian","slovenščina"},
    {"bs","bos","bosnian","bosanski"}, {"tr","tur","turkish","türkçe"},
    {"ca","cat","catalan","català"}, {"eu","eus","basque","euskara"},
    {"gl","glg","galician","galego"}, {"et","est","estonian","eesti"},
    {"lv","lav","latvian","latviešu"}, {"lt","lit","lithuanian","lietuvių"},
    {"hi","hin","hindi","हिन्दी"}, {"bn","ben","bengali","বাংলা"},
    {"ta","tam","tamil","தமிழ்"}, {"te","tel","telugu","తెలుగు"},
    {"kn","kan","kannada","ಕನ್ನಡ"}, {"ml","mal","malayalam","മലയാളം"},
    {"mr","mar","marathi","मराठी"}, {"ur","urd","urdu","اردو"},
    {"id","ind","indonesian","bahasa indonesia"}, {"ms","msa","malay","bahasa melayu"},
    {"vi","vie","vietnamese","tiếng việt"}, {"th","tha","thai","ไทย"},
    {"ja","jpn","japanese","日本語"}, {"ko","kor","korean","한국어"},
    {"zh","zho","chinese","中文"}, {"fa","fas","persian","فارسی"},
    {"sq","sqi","albanian","shqip"}, {"mk","mkd","macedonian","македонски"},
    {"hy","hye","armenian","հայերեն"}, {"ka","kat","georgian","ქართული"},
    {"af","afr","afrikaans","afrikaans"}, {"sw","swa","swahili","kiswahili"},
    {"tl","tgl","tagalog","filipino"}, {"nb","nob","bokmål","bokmal"},
    {"nn","nno","nynorsk","nynorsk"}
};
std::string code(std::string token) {
    token = lower(trim(token));
    if (token.compare(0, 2, "l_") == 0) token.erase(0, 2);
    if (token == "hi") return ""; // Common hearing-impaired marker; use hin/Hindi in titles.
    const auto dash = token.find('-');
    std::string base = token.substr(0, dash);
    const char *aliases[][2] = {{"fre","fr"},{"ger","de"},{"dut","nl"},{"cze","cs"},
        {"gre","el"},{"chi","zh"},{"rum","ro"},{"slo","sk"},{"baq","eu"},
        {"per","fa"},{"may","ms"},{"ice","is"},{"alb","sq"},{"mac","mk"}};
    for (const auto &a : aliases) if (base == a[0]) base = a[1];
    for (const auto &l : languages) if (base == l.code || base == l.iso3) {
        const std::string result = std::string(l.code) + (dash == std::string::npos ? "" : token.substr(dash));
        return usable(result) ? result : "";
    }
    return "";
}
bool containsWord(const std::string &text, const std::string &name) {
    size_t pos = 0;
    while ((pos = text.find(name, pos)) != std::string::npos) {
        if ((!pos || !word(text[pos - 1])) &&
            (pos + name.size() == text.size() || !word(text[pos + name.size()]))) return true;
        ++pos;
    }
    return false;
}
std::string description(const std::string &title) {
    std::string s = lower(trim(title)), found;
    bool ambiguous = false;
    auto add = [&](const std::string &lang) {
        if (lang.empty()) return;
        if (!found.empty() && found.substr(0, found.find('-')) != lang.substr(0, lang.find('-')))
            ambiguous = true;
        if (found.empty() || lang.find('-') != std::string::npos) found = lang;
    };
    add(code(s));
    for (const auto &l : languages)
        if (containsWord(s, l.name) || containsWord(s, l.alias)) add(l.code);
    // Bracketed ISO codes and l_XXX identifiers are explicit language markers.
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(' || s[i] == '[') {
            const auto end = s.find(s[i] == '(' ? ')' : ']', i + 1);
            if (end != std::string::npos) add(code(s.substr(i + 1, end - i - 1)));
        }
        if (s.compare(i, 2, "l_") == 0 && (!i || !word(s[i - 1]))) {
            const auto end = s.find_first_of(" )]", i);
            add(code(s.substr(i, end == std::string::npos ? end : end - i)));
        }
    }
    // A leading code followed exclusively by standard qualifiers, e.g. eng SDH.
    const auto first = s.find_first_of(" _(");
    if (first != std::string::npos) {
        std::string suffix = s.substr(first);
        for (char &c : suffix) if (!word(c)) c = ' ';
        std::istringstream words(suffix);
        std::string token;
        bool qualifiers = true;
        while (words >> token)
            if (token != "sdh" && token != "hi" && token != "cc" && token != "full" &&
                token != "forced" && token != "default" && token != "commentary") qualifiers = false;
        if (qualifiers) add(code(s.substr(0, first)));
    }
    return ambiguous ? "" : found;
}
void copyCode(char *out, size_t size, const std::string &s) {
    if (out && size) { const size_t n = std::min(size - 1, s.size()); std::memcpy(out, s.data(), n); out[n] = 0; }
}

// Reject invalid UTF-8 and keep complete code points at the output boundary.
size_t utf8(const unsigned char *p, size_t size) {
    if (p[0] < 0x80) return 1;
    size_t n = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 : p[0] >= 0xe0 && p[0] <= 0xef ? 3
             : p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!n || n > size) return 0;
    for (size_t i = 1; i < n; ++i) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0) ||
        (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90)) return 0;
    return n;
}
std::string clean(int format, const unsigned char *p, size_t size) {
    if (!p || !size || size > MAX_PACKET || format < SUB_LANG_TEXT || format > SUB_LANG_WEBVTT) return "";
    if (format == SUB_LANG_MOV_TEXT) {
        if (size < 2) return "";
        size_t len = (p[0] << 8) | p[1];
        if (len > size - 2) return "";
        p += 2; size = len;
    } else if (format == SUB_LANG_ASS) {
        unsigned commas = 0;
        while (size && commas < 8) { if (*p == ',') ++commas; ++p; --size; }
        if (commas != 8) return "";
    }
    std::string out;
    bool space = false;
    for (size_t i = 0; i < size;) {
        unsigned char c = p[i];
        if (c == '<' || (format == SUB_LANG_ASS && c == '{')) {
            const unsigned char end = c == '<' ? '>' : '}';
            while (i < size && p[i] != end) {
                if (format == SUB_LANG_ASS && p[i] == '\\' && i + 2 < size && p[i+1] == 'p' &&
                    p[i+2] >= '1' && p[i+2] <= '9') return "";
                ++i;
            }
            if (i < size) ++i;
            space = true;
            continue;
        }
        if (c == '&') {
            size_t end = i + 1;
            while (end < size && end - i <= 12 && p[end] != ';') ++end;
            if (end < size && p[end] == ';') { i = end + 1; space = true; continue; }
        }
        if (format == SUB_LANG_ASS && c == '\\' && i + 1 < size &&
            (p[i+1] == 'N' || p[i+1] == 'n' || p[i+1] == 'h')) { i += 2; space = true; continue; }
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { ++i; space = true; continue; }
        if (c < 32 || c == 127) return "";
        const size_t n = utf8(p + i, size - i);
        if (!n) return "";
        if (out.size() + n + (space && !out.empty()) > MAX_TEXT) break;
        if (space && !out.empty()) out += ' ';
        space = false;
        out.append(reinterpret_cast<const char *>(p + i), n);
        i += n;
    }
    return out;
}
uint64_t hash(const std::string &s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
struct Track {
    std::string text, candidate, language;
    std::array<uint64_t, 16> hashes{};
    int64_t lastPts = std::numeric_limits<int64_t>::min();
    unsigned cues = 0, seen = 0, attempts = 0;
    float previousScore = 0;
    int confidence = 0;
    bool ready = false, busy = false, done = false, result = false;
};
} // namespace

struct sub_language_session {
    std::mutex mutex;
    std::condition_variable wake;
    std::array<Track, MAX_TRACKS> tracks;
    std::thread worker;
    bool stop = false, failed = false;
};

#ifdef CONFIG_FASTTEXT
namespace {
struct Model {
    fasttext::FastText value;
    explicit Model(const std::string &path) { value.loadModel(path); ++loadedModels; }
    ~Model() { --loadedModels; }
};
void run(SUB_LANGUAGE_SESSION *s) {
    const unsigned MAX_ATTEMPTS = 8;
    std::unique_ptr<Model> model;
    auto lastUse = std::chrono::steady_clock::now();
    try {
        std::unique_lock<std::mutex> lock(s->mutex);
        while (!s->stop) {
            unsigned index = 0;
            while (index < MAX_TRACKS && !s->tracks[index].ready) ++index;
            if (index == MAX_TRACKS) {
                if (std::chrono::steady_clock::now() - lastUse >= std::chrono::seconds(5)) model.reset();
                s->wake.wait_for(lock, std::chrono::seconds(1));
                continue;
            }
            std::string path;
            { std::lock_guard<std::mutex> guard(pathMutex); path = modelPath; }
            if (path.empty()) {
                const char *env = std::getenv("AVOS_FASTTEXT_MODEL");
                if (env) path = env;
            }
            if (path.empty()) { s->wake.wait_for(lock, std::chrono::seconds(1)); continue; }
            Track &track = s->tracks[index];
            std::string sample;
            sample.swap(track.text);
            track.ready = false; track.busy = true; track.cues = 0;
            ++track.attempts;
            lock.unlock();
            if (!model) model.reset(new Model(path));
            std::istringstream input(sample);
            std::vector<std::pair<fasttext::real, std::string>> predictions;
            model->value.predictLine(input, predictions, 2, 0.0f);
            lastUse = std::chrono::steady_clock::now();
            lock.lock();
            track.busy = false;
            if (s->stop) break;
            const float score = predictions.empty() ? 0 : predictions[0].first;
            const float runnerUp = predictions.size() < 2 ? 1 : predictions[1].first;
            if (score >= 0.85f && score - runnerUp >= 0.20f &&
                predictions[0].second.compare(0, 9, "__label__") == 0) {
                const std::string language = predictions[0].second.substr(9);
                if (usable(language) && language == track.candidate) {
                    track.language = language;
                    track.confidence = static_cast<int>(std::min(1.0f, std::min(score, track.previousScore)) * 100);
                    track.result = track.done = true;
                } else {
                    track.candidate = usable(language) ? language : "";
                    track.previousScore = score;
                }
            } else {
                track.candidate.clear(); track.previousScore = 0;
            }
            if (track.attempts >= MAX_ATTEMPTS) track.done = true;
            if (track.done) { std::string().swap(track.text); std::string().swap(track.candidate); }
        }
    } catch (...) {
        // Missing/corrupt models or allocation failures must not escape into playback.
        std::lock_guard<std::mutex> guard(s->mutex);
        s->failed = true;
        for (auto &track : s->tracks) {
            track.busy = track.ready = false;
            track.done = true;
            std::string().swap(track.text);
        }
    }
}
} // namespace
#endif

extern "C" int sub_language_resolve(const char *metadata, const char *title, char *out, size_t size) {
    copyCode(out, size, "");
    try {
        const std::string language = lower(trim(metadata ? metadata : ""));
        if (usable(language)) { copyCode(out, size, language); return SUB_LANG_METADATA; }
        const std::string inferred = description(title ? title : "");
        if (!inferred.empty()) { copyCode(out, size, inferred); return SUB_LANG_DESCRIPTION; }
    } catch (...) { }
    return SUB_LANG_UNKNOWN;
}
extern "C" void sub_language_set_model_path(const char *path) {
    try { std::lock_guard<std::mutex> guard(pathMutex); modelPath = path ? path : ""; } catch (...) { }
}
extern "C" SUB_LANGUAGE_SESSION *sub_language_create(void) {
#ifdef CONFIG_FASTTEXT
    try { return new SUB_LANGUAGE_SESSION; } catch (...) { }
#endif
    return nullptr;
}
extern "C" void sub_language_destroy(SUB_LANGUAGE_SESSION *s) {
    if (!s) return;
    { std::lock_guard<std::mutex> guard(s->mutex); s->stop = true; s->wake.notify_all(); }
    if (s->worker.joinable()) s->worker.join();
    delete s;
}
extern "C" void sub_language_feed(SUB_LANGUAGE_SESSION *s, int index, int format,
                                   const void *data, size_t size, int64_t pts) {
    if (!s || index < 0 || index >= static_cast<int>(MAX_TRACKS) || pts == INT64_MIN) return;
    try {
        const std::string text = clean(format, static_cast<const unsigned char *>(data), size);
        if (text.empty()) return;
        std::lock_guard<std::mutex> guard(s->mutex);
        Track &track = s->tracks[index];
        if (s->stop || s->failed || track.done || track.ready || track.busy || pts <= track.lastPts) return;
        track.lastPts = pts;
        const uint64_t digest = hash(text);
        for (unsigned i = 0; i < std::min(track.seen, 16u); ++i) if (track.hashes[i] == digest) return;
        track.hashes[track.seen++ % 16] = digest;
        // An oversized single cue cannot satisfy the distinct-cue requirement.
        if (track.text.size() + text.size() + 1 <= MAX_TEXT) {
            if (!track.text.empty()) track.text += ' ';
            track.text += text; ++track.cues;
        } else if (track.cues < MIN_CUES) {
            track.text.clear(); track.cues = 0;
        }
        if (track.text.size() >= MIN_TEXT && track.cues >= MIN_CUES) {
            track.ready = true;
#ifdef CONFIG_FASTTEXT
            if (!s->worker.joinable()) s->worker = std::thread(run, s);
#endif
            s->wake.notify_one();
        }
    } catch (...) {
        std::lock_guard<std::mutex> guard(s->mutex);
        s->failed = true; s->stop = true; s->wake.notify_all();
    }
}
extern "C" int sub_language_poll(SUB_LANGUAGE_SESSION *s, int index, char *out, size_t size, int *confidence) {
    if (!s || index < 0 || index >= static_cast<int>(MAX_TRACKS)) return 0;
    std::lock_guard<std::mutex> guard(s->mutex);
    Track &track = s->tracks[index];
    if (!track.result) return 0;
    copyCode(out, size, track.language);
    if (confidence) *confidence = track.confidence;
    track.result = false;
    return 1;
}
extern "C" void sub_language_metadata_lock(void) { metadataMutex.lock(); }
extern "C" void sub_language_metadata_unlock(void) { metadataMutex.unlock(); }
extern "C" int sub_language_model_count(void) { return loadedModels.load(); }
extern "C" int sub_language_pending(SUB_LANGUAGE_SESSION *s) {
    if (!s) return 0;
    std::lock_guard<std::mutex> guard(s->mutex);
    int count = 0;
    for (const auto &track : s->tracks) if (track.ready || track.busy) ++count;
    return count;
}
extern "C" int sub_language_finished(SUB_LANGUAGE_SESSION *s, int index) {
    if (!s || index < 0 || index >= static_cast<int>(MAX_TRACKS)) return 1;
    std::lock_guard<std::mutex> guard(s->mutex);
    return s->failed || s->stop || s->tracks[index].done;
}
