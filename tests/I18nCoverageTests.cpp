#include <BinaryData.h>

#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

// German translation coverage as a MEASUREMENT, not an intent (issue #49).
//
// The failure this prevents from recurring: #45 added a user-facing notice
// sentence and shipped it with no German string - consistent with the #42
// strings, which were also missing - so a German-locale user got a mixed-
// language UI and nothing anywhere went red. The de.txt file said nothing
// about what it was supposed to cover, and no test measured it.
//
// THE #48 PATTERN: MEASURE THE SHIPPED BYTES, NOT THE INTENT. Two measured
// sides, nothing declared by hand:
//
//   - The GERMAN side is BinaryData::de_txt - the bytes actually compiled
//     into this binary, the same bytes src/presets/Localisation.cpp hands to
//     juce::LocalisedStrings. Not the file in the source tree: a de.txt that
//     was edited but not re-embedded is exactly a drift this must catch.
//
//   - The ENGLISH side is the TRANS()/juce::translate() string-literal table
//     extracted from src/ - the same sources this test binary was compiled
//     from (NAVE_SRC_DIR, the same source-tree-measuring pattern
//     NAVE_IR_ASSET_DIR established). TRANS()'s argument is a literal by
//     construction, so the extraction is the shipped string table.
//
// BOTH DIRECTIONS ARE RATCHETED. A TRANS() string without a de entry is the
// #49 regression itself; a de entry without a TRANS() string is a stale
// translation that would silently rot (and would hide a renamed English
// string behind a green test).
//
// WHAT IS DELIBERATELY OUT OF SCOPE. Parameter names, units and technical
// terms (LoCut, Hz, dB, ...) are never translated anywhere in this plugin -
// see src/presets/Localisation.h's scope note - so they are not in the
// TRANS() table and not measured here. A TRANS (someVariable) call would
// also escape the extraction; none exists, and the convention (enforced by
// review, stated here) is that TRANS() takes a literal precisely so the
// table stays measurable.
namespace
{
    // Extracts every TRANS ("...") / juce::translate ("...") string literal
    // from `text`, handling adjacent-literal concatenation across lines
    // (TRANS ("a " "b")) and backslash escapes. Occurrences of the keywords
    // that are not immediately a call with a literal argument (comments,
    // "already-TRANS()'d") contribute nothing.
    void extractTranslatedLiterals (const juce::String& text, std::set<juce::String>& into)
    {
        const auto raw = text.toStdString();

        for (const char* keyword : { "TRANS", "translate" })
        {
            const std::string needle (keyword);
            size_t i = 0;

            while ((i = raw.find (needle, i)) != std::string::npos)
            {
                size_t j = i + needle.size();
                i = j;

                const auto skipWhitespace = [&raw] (size_t at)
                {
                    while (at < raw.size() && (raw[at] == ' ' || raw[at] == '\t' || raw[at] == '\n' || raw[at] == '\r'))
                        ++at;
                    return at;
                };

                j = skipWhitespace (j);

                if (j >= raw.size() || raw[j] != '(')
                    continue;

                ++j;

                std::string literal;
                bool sawLiteral = false;

                // One or more adjacent string literals, exactly as the
                // compiler would concatenate them.
                while (true)
                {
                    j = skipWhitespace (j);

                    if (j >= raw.size() || raw[j] != '"')
                        break;

                    ++j;
                    sawLiteral = true;

                    while (j < raw.size())
                    {
                        const char c = raw[j];

                        if (c == '\\' && j + 1 < raw.size())
                        {
                            // The only escapes the shipped strings use; a
                            // new one failing loudly here beats a silently
                            // wrong key.
                            const char escaped = raw[j + 1];
                            REQUIRE ((escaped == '"' || escaped == '\\'));
                            literal += escaped;
                            j += 2;
                            continue;
                        }

                        if (c == '"')
                        {
                            ++j;
                            break;
                        }

                        literal += c;
                        ++j;
                    }
                }

                if (sawLiteral)
                    into.insert (juce::String::fromUTF8 (literal.c_str()));
            }
        }
    }

    std::set<juce::String> shippedTranslatableStrings()
    {
        const juce::File sourceRoot (juce::String (NAVE_SRC_DIR));
        REQUIRE (sourceRoot.isDirectory());

        std::set<juce::String> strings;
        int filesScanned = 0;

        for (const auto& entry : juce::RangedDirectoryIterator (sourceRoot, true, "*.cpp;*.h", juce::File::findFiles))
        {
            ++filesScanned;
            extractTranslatedLiterals (entry.getFile().loadFileAsString(), strings);
        }

        // If the scan found no sources the measurement is not a measurement.
        REQUIRE (filesScanned > 0);
        REQUIRE (! strings.empty());

        return strings;
    }

    // The embedded de.txt, parsed line-by-line into its keys and values.
    // Parsed directly rather than through juce::LocalisedStrings because a
    // missing mapping and an identity mapping ("IR A" = "IR A") are
    // indistinguishable through translate() - and identity mappings are
    // legitimate (proper nouns, shared terms), so the KEYS are the coverage
    // measure.
    struct GermanTable
    {
        std::set<juce::String> keys;
        int duplicateKeys = 0;
        juce::String raw;
    };

    GermanTable shippedGermanTable()
    {
        GermanTable table;
        table.raw = juce::String::fromUTF8 (BinaryData::de_txt,
                                            BinaryData::de_txtSize);

        for (const auto& line : juce::StringArray::fromLines (table.raw))
        {
            const auto trimmed = line.trim();

            if (! trimmed.startsWithChar ('"'))
                continue; // the language/countries header, blanks, comments

            const auto separator = trimmed.indexOf ("\" = \"");
            REQUIRE (separator > 0); // a quoted line that is not a mapping is a typo

            const auto key = trimmed.substring (1, separator);

            if (! table.keys.insert (key).second)
                ++table.duplicateKeys;
        }

        return table;
    }
}

TEST_CASE ("i18n: every user-facing string has a German entry, measured from the shipped bytes",
           "[i18n][presets][content]")
{
    const auto strings = shippedTranslatableStrings();
    const auto german = shippedGermanTable();

    // The header juce::LocalisedStrings keys the whole table on.
    CHECK (german.raw.startsWith ("language: German"));
    CHECK (german.raw.contains ("countries: de"));
    CHECK (german.duplicateKeys == 0);

    // Direction 1 - the #49 regression itself: a string the UI can show that
    // the German table does not know. The INFO names the exact string, so
    // the fix is mechanical: add the entry to resources/i18n/de.txt.
    for (const auto& string : strings)
    {
        INFO ("TRANS()'d string with no de.txt entry: \"" << string.toStdString() << "\"");
        CHECK (german.keys.count (string) == 1);
    }

    // Direction 2 - staleness: a German entry for a string the sources no
    // longer contain. This is what catches an English string being reworded
    // while its old translation keeps the table looking complete.
    for (const auto& key : german.keys)
    {
        INFO ("de.txt entry with no TRANS()'d string in src/: \"" << key.toStdString() << "\"");
        CHECK (strings.count (key) == 1);
    }

    // The two measures agree in size, which the two loops above already
    // imply - stated once more as the single number a failure log shows
    // first.
    CHECK (strings.size() == german.keys.size());
}

TEST_CASE ("i18n: the shipped German table actually translates through juce::LocalisedStrings",
           "[i18n][presets]")
{
    // The coverage test above proves the KEYS are right; this proves the
    // FILE works as a juce::LocalisedStrings document at all - a malformed
    // line could carry a perfectly measurable key and still fail to load.
    // JUCE 8.0.14, juce_LocalisedStrings.h: construction parses the whole
    // document, translate() falls through to the input for unknown text.
    const juce::LocalisedStrings german (juce::String::fromUTF8 (BinaryData::de_txt,
                                                                 BinaryData::de_txtSize),
                                         false);

    CHECK (german.getLanguageName() == "German");

    // A mapping that genuinely differs, so this cannot pass via fallthrough.
    CHECK (german.translate ("Factory") == juce::String::fromUTF8 ("Werksvoreinstellungen"));

    // The #49 sentence itself: present, German, and still carrying the NAMES
    // placeholder the caller substitutes after translation.
    const auto missSingular =
        german.translate ("This preset was made with NAMES, which is not in your IR library.");
    CHECK (missSingular != "This preset was made with NAMES, which is not in your IR library.");
    CHECK (missSingular.contains ("NAMES"));

    // Unknown text falls through unchanged - the documented behaviour the
    // English-locale path relies on.
    CHECK (german.translate ("not a key anywhere") == "not a key anywhere");
}
