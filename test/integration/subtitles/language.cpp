// Copyright 2026 Courville Software
// SPDX-License-Identifier: Apache-2.0
#include "sub_language.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

static void resolve(const char *metadata, const char *title, int source, const char *expected) {
    char result[64];
    assert(sub_language_resolve(metadata, title, result, sizeof(result)) == source);
    assert(std::string(result) == expected);
}
static void idle(SUB_LANGUAGE_SESSION *s) {
    for (int i = 0; i < 300 && sub_language_pending(s); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!sub_language_pending(s));
}
static const char *sentences[] = {
    "Nous devons partir avant le coucher du soleil pour retrouver nos amis dans la vallée.",
    "Je ne comprends pas pourquoi tu refuses de nous accompagner pendant ce voyage.",
    "La prochaine fois, nous prendrons le train plutôt que de traverser toute la ville à pied.",
    "Tu pourrais fermer les fenêtres et vérifier que la porte de la maison est bien verrouillée.",
    "Il faudrait demander à notre voisine si elle peut surveiller les enfants demain après-midi.",
    "Nous avons attendu longtemps devant la gare, mais personne ne nous a répondu.",
    "Elle savait déjà que son frère avait décidé de quitter le village pour travailler ailleurs.",
    "Pourquoi ne pas préparer le dîner ensemble pendant que les autres terminent leurs devoirs ?",
    "Quand nous sommes arrivés, tous les magasins étaient fermés et les rues étaient désertes.",
    "J'aimerais savoir ce que tu penses de cette histoire avant de prendre une décision.",
    "Vous devriez prendre votre temps pour réfléchir aux conséquences de cette proposition.",
    "Les enfants avaient oublié leurs affaires dans la voiture qui était garée devant l'école.",
    "Je vous remercie pour votre aide, sans vous nous n'aurions jamais trouvé notre chemin.",
    "Personne ne savait comment expliquer ce qui venait de se passer dans cette petite maison.",
    "Nous pourrions nous retrouver demain matin pour discuter tranquillement de ce problème.",
    "Avant de commencer, il faut vérifier que chacun a bien compris les instructions du professeur."
};
int main(int argc, char **argv) {
    assert(argc == 2);
    resolve("eng", "French", SUB_LANG_METADATA, "eng");
    resolve("pt-BR", "", SUB_LANG_METADATA, "pt-br");
    resolve("und", "French (SDH)", SUB_LANG_DESCRIPTION, "fr");
    resolve("", "Français", SUB_LANG_DESCRIPTION, "fr");
    resolve("", "eng SDH", SUB_LANG_DESCRIPTION, "en");
    resolve("", "commentary (l_fre)", SUB_LANG_DESCRIPTION, "fr");
    resolve("", "Japanese / English", SUB_LANG_UNKNOWN, "");
    resolve("und", "TEXT", SUB_LANG_UNKNOWN, "");
    resolve("", "SDH", SUB_LANG_UNKNOWN, "");
    resolve("", "HI", SUB_LANG_UNKNOWN, "");
    resolve("", "Full", SUB_LANG_UNKNOWN, "");
    sub_language_set_model_path(argv[1]);
    auto *s = sub_language_create();
    assert(s && sub_language_model_count() == 0);
    assert(!sub_language_failed(s));
    // Repeated text, malformed UTF-8, oversized packets and a sparse track do not load a model.
    std::string large(20000, 'a');
    for (int i = 0; i < 24; ++i) {
        sub_language_feed(s, 1, SUB_LANG_TEXT, sentences[0], std::strlen(sentences[0]), i);
        sub_language_feed(s, 2, SUB_LANG_TEXT, "\xc0\xaf", 2, i);
        sub_language_feed(s, 3, SUB_LANG_TEXT, large.data(), large.size(), i);
    }
    assert(sub_language_model_count() == 0);
    for (int format : {SUB_LANG_TEXT, SUB_LANG_ASS, SUB_LANG_MOV_TEXT, SUB_LANG_WEBVTT}) {
        for (int window = 0; window < 2; ++window) {
            for (int cue = 0; cue < 8; ++cue) {
                std::string text = std::string("<i>") + sentences[window * 8 + cue] + "</i>";
                if (format == SUB_LANG_ASS) text = "0,0,Default,,0,0,0,,{\\i1}" + text;
                if (format == SUB_LANG_MOV_TEXT) {
                    const size_t n = text.size();
                    text = std::string(1, char(n >> 8)) + char(n & 255) + text;
                }
                sub_language_feed(s, format + 8, format, text.data(), text.size(), window * 8 + cue);
            }
            idle(s);
            if (!window) {
                char language[32]; int confidence;
                assert(!sub_language_poll(s, format + 8, language, sizeof(language), &confidence));
                // Seeking back must not turn the same evidence into a second sample.
                for (int cue = 0; cue < 8; ++cue)
                    sub_language_feed(s, format + 8, SUB_LANG_TEXT, sentences[cue], std::strlen(sentences[cue]), cue);
                assert(!sub_language_pending(s));
            }
        }
        char language[32]; int confidence;
        assert(sub_language_poll(s, format + 8, language, sizeof(language), &confidence));
        assert(std::string(language) == "fr" && confidence >= 85 && confidence <= 100);
        assert(!sub_language_poll(s, format + 8, language, sizeof(language), &confidence));
        std::cout << "format " << format << ": French (AI " << confidence << "%)\n";
    }
    assert(sub_language_model_count() == 1);
    // Two confident but different samples must not publish a result.
    const std::string english = "We should leave before sunset and ask our friends to bring their children. "
        "Please remember to close the windows and lock the door before you go. ";
    for (int window = 0; window < 4; ++window) {
        for (int cue = 0; cue < 8; ++cue) {
            std::string text = window == 1 ? english + std::to_string(cue)
                    : sentences[(window == 2 ? 8 : 0) + cue];
            sub_language_feed(s, 30, SUB_LANG_TEXT, text.data(), text.size(), window * 8 + cue);
        }
        idle(s);
        char language[32]; int confidence;
        const int result = sub_language_poll(s, 30, language, sizeof(language), &confidence);
        assert(result == (window == 3));
        if (result) assert(std::string(language) == "fr");
    }
    // Non-language text must remain unknown and stop consuming jobs after eight attempts.
    for (int window = 0; window < 9; ++window) {
        for (int cue = 0; cue < 8; ++cue) {
            std::string text = "zxqv jklq wqzz vvvx zqjk qzxw qqqz xqkv wjzx vqkj qxxz zjxq qkzx vqww "
                    + std::to_string(window * 8 + cue);
            sub_language_feed(s, 31, SUB_LANG_TEXT, text.data(), text.size(), window * 8 + cue);
        }
        if (window == 8) assert(!sub_language_pending(s));
        idle(s);
        char language[32]; int confidence;
        assert(!sub_language_poll(s, 31, language, sizeof(language), &confidence));
    }
    for (int i = 0; i < 700 && sub_language_model_count(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(sub_language_model_count() == 0);
    sub_language_destroy(s);
    // Closing with jobs queued must cancel and release without waiting for idle expiry.
    s = sub_language_create();
    for (int cue = 0; cue < 8; ++cue)
        sub_language_feed(s, 0, SUB_LANG_TEXT, sentences[cue], std::strlen(sentences[cue]), cue);
    sub_language_destroy(s);
    assert(sub_language_model_count() == 0);
    // A missing model is nonfatal and its session can be closed safely.
    sub_language_set_model_path("/does-not-exist/lid.176.ftz");
    s = sub_language_create();
    for (int cue = 0; cue < 8; ++cue)
        sub_language_feed(s, 0, SUB_LANG_TEXT, sentences[cue], std::strlen(sentences[cue]), cue);
    idle(s);
    assert(sub_language_failed(s));
    sub_language_destroy(s);
    assert(sub_language_model_count() == 0);
}
