// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "common/utils/peer_code.h"

#include "monocypher.h"

#include <cstdio>
#include <cstring>

namespace vivora::peer_code {

namespace {

// 128 adjectives — neutral / friendly tone, 4–7 chars each.  Indexed by 7
// bits of the pubkey hash.  ORDER IS WIRE-FORMAT — never reorder or remove,
// only append (and bump VERSION) if the layout changes.
constexpr const char* ADJ[128] = {
    "agile",   "amber",   "ancient", "autumn",  "azure",   "bashful", "bold",    "brave",
    "brisk",   "bright",  "bronze",  "burly",   "busy",    "calm",    "candid",  "careful",
    "charm",   "cheery",  "civic",   "clean",   "clear",   "clever",  "cobalt",  "cool",
    "copper",  "cosmic",  "cozy",    "crimson", "crisp",   "daily",   "dapper",  "dark",
    "deft",    "dewy",    "dizzy",   "dreamy",  "dusky",   "eager",   "early",   "elated",
    "elegant", "epic",    "faint",   "fair",    "fancy",   "fast",    "fierce",  "filmy",
    "final",   "fine",    "firm",    "first",   "flat",    "fluffy",  "fond",    "frank",
    "free",    "fresh",   "frosty",  "fuzzy",   "gentle",  "glad",    "glassy",  "glossy",
    "golden",  "grand",   "gray",    "great",   "green",   "happy",   "hasty",   "hazel",
    "heavy",   "hidden",  "honest",  "humble",  "husky",   "icy",     "jolly",   "joyful",
    "kind",    "lazy",    "lemon",   "light",   "lively",  "loud",    "loyal",   "lucky",
    "lush",    "magic",   "marble",  "merry",   "mighty",  "mild",    "misty",   "modern",
    "mossy",   "neat",    "nice",    "noble",   "nutty",   "odd",     "olive",   "opal",
    "orange",  "peach",   "pearl",   "pink",    "plain",   "plump",   "plush",   "polar",
    "proud",   "pure",    "purple",  "quick",   "quiet",   "ready",   "red",     "regal",
    "rich",    "ripe",    "rosy",    "royal",   "ruby",    "rugged",  "rusty",   "sandy",
};

// 128 nouns — concrete things (animals, plants, terrain, weather), 3–7 chars.
constexpr const char* NOUN[128] = {
    "acorn",   "anchor",  "apple",   "arrow",   "badger",  "banner",  "basket",  "beach",
    "bear",    "bell",    "berry",   "bird",    "boat",    "book",    "branch",  "bread",
    "breeze",  "brook",   "butter",  "cactus",  "camel",   "candle",  "canyon",  "cat",
    "cave",    "cedar",   "cherry",  "cliff",   "cloud",   "clover",  "comet",   "copper",
    "coral",   "cougar",  "creek",   "crow",    "crystal", "daisy",   "deer",    "desert",
    "dolphin", "dragon",  "dune",    "dusk",    "eagle",   "ember",   "falcon",  "feather",
    "fern",    "finch",   "flame",   "fog",     "forest",  "fox",     "frog",    "garden",
    "gem",     "glacier", "granite", "grass",   "grove",   "gull",    "harbor",  "hare",
    "hawk",    "heron",   "hill",    "honey",   "ivy",     "jaguar",  "jasmin",  "juniper",
    "lake",    "leaf",    "lemon",   "lily",    "lion",    "lotus",   "lynx",    "maple",
    "meadow",  "mist",    "moon",    "moose",   "mouse",   "oak",     "ocean",   "opal",
    "orchid",  "otter",   "owl",     "panda",   "peach",   "pebble",  "petal",   "pine",
    "plum",    "pond",    "poppy",   "rain",    "raven",   "reef",    "river",   "robin",
    "rose",    "salmon",  "sea",     "seal",    "shore",   "sky",     "snow",    "sparrow",
    "spruce",  "star",    "stone",   "storm",   "stream",  "summit",  "sun",     "swan",
    "tiger",   "topaz",   "tower",   "tree",    "tulip",   "valley",  "violet",  "willow",
};

inline bool is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// Cheap word-existence check — linear scan of the 128-entry list. Fine for
// the lookup-by-code path; the rendezvous server hits this once per LOOKUP
// and the cost is negligible compared to the network round-trip.
int find_word(const char* const list[128], const char* word, size_t len) {
    for (int i = 0; i < 128; ++i) {
        if (std::strlen(list[i]) == len && std::memcmp(list[i], word, len) == 0) {
            return i;
        }
    }
    return -1;
}

} // namespace

size_t encode(const uint8_t pubkey[32], char out[MAX_CODE_LEN]) {
    // 8-byte BLAKE2b digest is enough — we only consume the low 32 bits.
    uint8_t h[8];
    crypto_blake2b(h, sizeof(h), pubkey, 32);
    const uint32_t bits = static_cast<uint32_t>(h[0])
                        | (static_cast<uint32_t>(h[1]) <<  8)
                        | (static_cast<uint32_t>(h[2]) << 16)
                        | (static_cast<uint32_t>(h[3]) << 24);
    const int adj_idx  = static_cast<int>( bits        & 0x7f);  // 7 bits
    const int noun_idx = static_cast<int>((bits >>  7) & 0x7f);  // 7 bits
    const int number   = static_cast<int>((bits >> 14) % 10000); // ~14 bits, mod 10000

    // snprintf reserves space for the trailing '\0', total written is ret.
    int n = std::snprintf(out, MAX_CODE_LEN, "%s-%s-%04d",
                          ADJ[adj_idx], NOUN[noun_idx], number);
    if (n < 0 || static_cast<size_t>(n) >= MAX_CODE_LEN) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

std::string encode(const uint8_t pubkey[32]) {
    char buf[MAX_CODE_LEN];
    const size_t n = encode(pubkey, buf);
    return std::string(buf, n);
}

bool is_well_formed(const char* s) {
    if (!s) return false;
    const size_t total = std::strlen(s);
    if (total < 7 || total >= MAX_CODE_LEN) return false;  // adj-noun-NNNN minimum

    // Find the two '-' separators.  Walk from the end: last 5 chars must be
    // "-NNNN".  Then a single '-' between adj and noun.
    if (s[total - 5] != '-') return false;
    for (size_t i = total - 4; i < total; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    const char* dash = nullptr;
    for (size_t i = 0; i < total - 5; ++i) {
        if (s[i] == '-') {
            if (dash) return false;  // more than one mid-dash
            dash = s + i;
        }
    }
    if (!dash) return false;
    const size_t adj_len  = static_cast<size_t>(dash - s);
    const size_t noun_len = static_cast<size_t>((s + total - 5) - (dash + 1));
    if (adj_len  == 0 || noun_len == 0) return false;
    if (find_word(ADJ,  s,        adj_len)  < 0) return false;
    if (find_word(NOUN, dash + 1, noun_len) < 0) return false;
    return true;
}

bool looks_like_hex_pubkey(const char* s) {
    if (!s) return false;
    if (std::strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        if (!is_lower_hex(s[i])) return false;
    }
    return true;
}

} // namespace vivora::peer_code
