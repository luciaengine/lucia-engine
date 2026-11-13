// What a JSON Lines store load reads from each line: the records, redirects,
// quarantined and displaced rows, and the error text of every malformed line
// of a varied store; and the allocations a line costs.

#include <gtest/gtest.h>

#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "Memory/AllocationCountScope.h"
#include "TestTempDir.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::AssetDatabase;

namespace
{

std::string Guid(int n)
{
    char text[40];
    std::snprintf(text, sizeof(text), "aaaaaaaa-0000-4000-8000-%012x", n);
    return text;
}

void WriteLines(const std::filesystem::path& file, const std::vector<std::string>& lines)
{
    std::ofstream out(file, std::ios::binary);
    for (const std::string& line : lines)
        out << line << '\n';
}

std::string Escaped(std::string_view text)
{
    std::string out;
    for (const char c : text)
        out += c == '\n' ? std::string("\\n") : std::string(1, c);
    return out;
}

std::string DescribeRecord(const AssetRecord& record)
{
    std::vector<std::string> members;
    for (const auto& [key, value] : record.kv)
        members.push_back(Escaped(key) + "=" + Escaped(value));
    std::sort(members.begin(), members.end());
    std::string text = record.guid.ToString() + " path=" + Escaped(record.path) + " typeId=" + record.typeId +
                       " type=" + std::to_string(static_cast<int>(record.type)) +
                       " missing=" + (record.missing ? "1" : "0") + " kv{";
    for (const std::string& member : members)
        text += member + ";";
    return text + "}";
}

// Everything a load leaves behind, one row per line, in a stable order.
std::string DescribeLoad(AssetStore_TextJsonl& store)
{
    std::vector<std::string> rows;
    for (const AssetRecord& record : store.EnumerateAssets())
        rows.push_back("asset " + DescribeRecord(record));
    for (const RedirectRecord& redirect : store.EnumerateRedirects())
        rows.push_back("redirect " + redirect.from.ToString() + " -> " + redirect.to.ToString());
    for (const AssetRecord& record : store.GetLoadQuarantinedRecords())
        rows.push_back("quarantined " + DescribeRecord(record));
    for (const AssetRecord& record : store.TakeDisplacedRecords())
        rows.push_back("displaced " + DescribeRecord(record));
    std::sort(rows.begin(), rows.end());
    for (const std::string& conflict : store.GetLoadConflicts())
        rows.push_back("conflict " + Escaped(conflict));
    std::string text;
    for (const std::string& row : rows)
        text += row + "\n";
    return text;
}

// Version-1 lines first (merge conflicts), then a version-2 journal with
// escapes, duplicate and unknown keys, wrong types and syntax errors.
std::vector<std::string> VariedStoreLines()
{
    const auto g = [](int n) { return "\"" + Guid(n) + "\""; };
    return {
        "# a comment",
        "   # an indented comment",
        "",
        "{\"guid\":" + g(40) + ",\"path\":\"V1/a.bin\",\"kv\":{\"k\":\"1\"}}",
        "{\"guid\":" + g(40) + ",\"path\":\"V1/b.bin\",\"kv\":{\"k\":\"2\",\"k2\":\"x\"}}",
        "{\"guid\":" + g(41) + ",\"path\":\"V1/a.bin\"}",
        "{\"redirect_from\":" + g(42) + ",\"redirect_to\":" + g(43) + "}",
        "{\"redirect_from\":" + g(42) + ",\"redirect_to\":" + g(44) + "}",
        "{\"format\":\"assetdb\",\"version\":2}",
        "{\"guid\":" + g(1) + ",\"path\":\"Textures/a.png\",\"type\":\"Texture\"}",
        "{\"guid\":" + g(2) + ",\"path\":\"Models\\\\b.glb\",\"type\":\"Model\",\"missing\":true,\"kv\":{\"k1\":\"v1\","
            "\"k2\":\"line\\nbreak \\\"quoted\\\" \\u0041\",\"empty\":\"\",\"\":\"empty key\",\"num\":5,"
            "\"obj\":{\"x\":\"y\"},\"arr\":[\"z\"]}}",
        "{\"guid\":" + g(3) + ",\"path\":\"Shaders/c.hlsl\",\"type\":\"MyPlugin.CustomType\","
            "\"extra\":{\"nested\":[1,2,{\"deep\":\"x\",\"guid\":\"inner\"}]},\"unknown\":null}",
        "{\"guid\":" + g(4) + ",\"path\":\"Audio/d.wav\",\"type\":7}",
        "{\"guid\":" + g(5) + ",\"path\":\"Misc/e.bin\"}",
        "{\"guid\":" + g(6) + ",\"path\":\"Misc/f.bin\",\"type\":3.5,\"missing\":1}",
        "{\"guid\":" + g(7) + ",\"path\":\"Misc/g.bin\",\"missing\":0,"
            "\"kv\":{\"dup\":\"first\",\"dup\":\"second\",\"gone\":\"x\",\"gone\":5}}",
        "{\"guid\":" + g(8) + ",\"path\":\"Misc/h.bin\",\"kv\":{\"a\":\"1\"},\"kv\":{\"b\":\"2\"}}",
        "{\"guid\":" + g(9) + ",\"path\":\"Misc/i.bin\",\"kv\":\"not an object\"}",
        "{\"guid\":" + g(10) + ",\"path\":\"Misc/j.bin\",\"guid\":" + g(11) + "}",
        "{\"guid\":\"not-a-guid\",\"path\":\"Misc/k.bin\"}",
        "{\"guid\":5,\"path\":\"Misc/l.bin\"}",
        "{\"guid\":" + g(12) + ",\"path\":[\"x\"]}",
        "{\"guid\":" + g(13) + ",\"path\":null}",
        "{\"guid\":" + g(14) + "}",
        "{\"guid\":" + g(15) + ",\"path\":\"Misc/m.bin\"",
        "{\"guid\":" + g(16) + ",\"path\":\"Misc/n.bin\"} trailing",
        "{\"guid\":" + g(17) + ",\"path\":\"Misc/o.bin\",}",
        "not json at all",
        "[1,2,3]",
        "\"just a string\"",
        "42",
        "{\"redirect_from\":" + g(20) + ",\"redirect_to\":" + g(21) + "}",
        "{\"redirect_from\":" + g(42) + ",\"redirect_to\":\"\"}",
        "{\"redirect_from\":" + g(22) + ",\"redirect_to\":5}",
        "{\"redirect_from\":\"bad\",\"redirect_to\":5}",
        "{\"redirect_from\":" + g(23) + ",\"redirect_to\":" + g(24) + ",\"guid\":" + g(25) +
            ",\"path\":\"Misc/p.bin\"}",
        "{\"guid\":" + g(3) + ",\"deleted\":true}",
        "{\"guid\":" + g(4) + ",\"deleted\":1}",
        "{\"guid\":" + g(5) + ",\"deleted\":false,\"path\":\"Misc/e2.bin\"}",
        "{\"guid\":" + g(6) + ",\"deleted\":\"yes\",\"path\":\"Misc/f2.bin\"}",
        "{\"guid\":" + g(9) + ",\"path\":\"C:/abs/q.bin\"}",
        "{\"guid\":" + g(26) + ",\"path\":\"textures/A.PNG\"}",
        "{\"guid\":" + g(27) + ",\"path\":\"Misc/big.bin\",\"missing\":4294967296}",
        "{\"guid\":" + g(28) + ",\"path\":\"Misc/negative.bin\",\"type\":-1}",
        "{\"format\":\"assetmanifest\",\"version\":0}",
        "{\"format\":\"other\",\"version\":3}",
        "{\"guid\":" + g(29) + ",\"path\":\"Misc/\\u0041.bin\",\"type\":\"Tex\\u0074ure\"}",
        "{\"gu\\u0069d\":" + g(30) + ",\"path\":\"Misc/escaped-key.bin\"}",
        "{\"guid\":" + g(31) + ",\"path\":\"Misc/overflow.bin\",\"type\":1e400}",
        "{\"guid\":" + g(32) + ",\"path\":\"Misc/bad\\u00.bin\"}",
        "  {\"guid\":" + g(33) + ",\"path\":\"Misc/indented.bin\",\"type\":\"Texture\"}\r",
    };
}

constexpr const char* kVariedStoreLoad = R"load(
asset aaaaaaaa-0000-4000-8000-000000000002 path=Models/b.glb typeId=Model type=3 missing=1 kv{empty=;k1=v1;k2=line\nbreak "quoted" A;}
asset aaaaaaaa-0000-4000-8000-000000000005 path=Misc/e2.bin typeId=Unknown type=0 missing=0 kv{}
asset aaaaaaaa-0000-4000-8000-000000000006 path=Misc/f2.bin typeId=Unknown type=0 missing=0 kv{}
asset aaaaaaaa-0000-4000-8000-000000000007 path=Misc/g.bin typeId=Unknown type=0 missing=0 kv{dup=second;}
asset aaaaaaaa-0000-4000-8000-000000000008 path=Misc/h.bin typeId=Unknown type=0 missing=0 kv{b=2;}
asset aaaaaaaa-0000-4000-8000-00000000000b path=Misc/j.bin typeId=Unknown type=0 missing=0 kv{}
asset aaaaaaaa-0000-4000-8000-00000000001a path=textures/A.PNG typeId=Unknown type=0 missing=0 kv{}
asset aaaaaaaa-0000-4000-8000-00000000001b path=Misc/big.bin typeId=Unknown type=0 missing=0 kv{}
asset aaaaaaaa-0000-4000-8000-00000000001c path=Misc/negative.bin typeId=Unknown type=-1 missing=0 kv{}
asset aaaaaaaa-0000-4000-8000-00000000001d path=Misc/A.bin typeId=Texture type=1 missing=0 kv{}
asset aaaaaaaa-0000-4000-8000-00000000001e path=Misc/escaped-key.bin typeId=Unknown type=0 missing=0 kv{}
asset aaaaaaaa-0000-4000-8000-000000000021 path=Misc/indented.bin typeId=Texture type=1 missing=0 kv{}
asset aaaaaaaa-0000-4000-8000-000000000028 path=V1/a.bin typeId=Unknown type=0 missing=0 kv{k2=x;k=1;}
displaced aaaaaaaa-0000-4000-8000-000000000001 path=Textures/a.png typeId=Texture type=1 missing=0 kv{}
displaced aaaaaaaa-0000-4000-8000-000000000029 path=V1/a.bin typeId=Unknown type=0 missing=0 kv{}
quarantined aaaaaaaa-0000-4000-8000-000000000009 path=C:/abs/q.bin typeId=Unknown type=0 missing=0 kv{}
redirect aaaaaaaa-0000-4000-8000-000000000014 -> aaaaaaaa-0000-4000-8000-000000000015
redirect aaaaaaaa-0000-4000-8000-000000000017 -> aaaaaaaa-0000-4000-8000-000000000018
conflict GUID conflict at line 5: aaaaaaaa-0000-4000-8000-000000000028 maps to multiple paths ('V1/a.bin' vs 'V1/b.bin')
conflict KV conflict for GUID aaaaaaaa-0000-4000-8000-000000000028 key 'k' at line 5
conflict Path conflict at line 6: 'V1/a.bin' maps to multiple GUIDs (aaaaaaaa-0000-4000-8000-000000000028 vs aaaaaaaa-0000-4000-8000-000000000029)
conflict Redirect conflict at line 8: from aaaaaaaa-0000-4000-8000-00000000002a maps to multiple targets
conflict Parse error at line 21: [json.exception.type_error.302] type must be string, but is number
conflict Parse error at line 22: [json.exception.type_error.302] type must be string, but is array
conflict Parse error at line 23: [json.exception.type_error.302] type must be string, but is null
conflict Parse error at line 25: [json.exception.parse_error.101] parse error at line 1, column 67: syntax error while parsing object - unexpected end of input; expected '}'
conflict Parse error at line 26: [json.exception.parse_error.101] parse error at line 1, column 71: syntax error while parsing value - invalid literal; last read: '"Misc/n.bin"} tra'; expected end of input
conflict Parse error at line 27: [json.exception.parse_error.101] parse error at line 1, column 68: syntax error while parsing object key - unexpected '}'; expected string literal
conflict Parse error at line 28: [json.exception.parse_error.101] parse error at line 1, column 2: syntax error while parsing value - invalid literal; last read: 'no'
conflict Parse error at line 34: [json.exception.type_error.302] type must be string, but is number
conflict Parse error at line 49: [json.exception.out_of_range.406] number overflow parsing '1e400'
conflict Parse error at line 50: [json.exception.parse_error.101] parse error at line 1, column 68: syntax error while parsing value - invalid string: '\u' must be followed by 4 hex digits; last read: '"Misc/bad\u00.'
)load";

// A record line shaped like a project's: a GUID, a path and a type. A rich
// line adds kKvMembers kv members and an unknown key with nested content.
// Every key and value is longer than any short-string buffer and shorter
// than the path.
constexpr int kRecords = 400;
constexpr int kKvMembers = 16;

std::string RecordLine(int i, bool rich)
{
    char hex[24];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(i) * 0x9E3779B97F4A7C15ull);
    std::string line = "{\"guid\":\"" + Guid(i) + "\",\"path\":\"Textures/LakesideKit/team/" + hex + "_" +
                       std::to_string(i) + ".png\",\"type\":\"Texture\"";
    if (!rich)
        return line + "}";
    line += ",\"extra\":[";
    for (int e = 0; e < 8; ++e)
        line += std::string(e == 0 ? "" : ",") + "{\"a\":1,\"b\":\"x\"}";
    line += "],\"kv\":{";
    for (int m = 0; m < kKvMembers; ++m)
    {
        char member[96];
        std::snprintf(member, sizeof(member), "%s\"assets.custom.member_%02d\":\"value-of-member-%02d-%05d\"",
                      m == 0 ? "" : ",", m, m, i);
        line += member;
    }
    return line + "}}";
}

std::uint64_t CountLoadAllocations(const std::filesystem::path& file)
{
    AssetStore_TextJsonl store(nullptr);
    std::string error;
    Memory::AllocationCountScope scope(Memory::CountWindow::ThisThread);
    const bool loaded = store.LoadFromFile(file, &error);
    const std::uint64_t count = scope.Count();
    EXPECT_TRUE(loaded) << error;
    EXPECT_EQ(store.CountAssets(), static_cast<size_t>(kRecords));
    return count;
}

} // namespace

TEST(AssetDbStoreLoad, AVariedStoreLoadsTheRecordsAndErrorsOfItsLines)
{
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("ge_assetdb_varied_store"));
    const std::filesystem::path dbFile = root.Path() / "AssetDatabase.assetdb";
    WriteLines(dbFile, VariedStoreLines());

    AssetStore_TextJsonl store(nullptr);
    std::string error;
    ASSERT_TRUE(store.LoadFromFile(dbFile, &error)) << error;
    const std::string actual = DescribeLoad(store);
    EXPECT_EQ(actual, std::string(kVariedStoreLoad).substr(1));
}

// A kv member costs at most its node, key and value, and a key the store
// does not read costs nothing: a load builds no JSON tree of the line and
// moves the parsed record into the store.
TEST(AssetDbStoreLoad, AKvMemberCostsItsNodeKeyAndValueAndAnUnknownKeyNothing)
{
    const TestUtils::ScopedTempDir root(TestUtils::MakeUniqueTempDirectory("ge_assetdb_load_allocations"));
    std::vector<std::string> plainLines{"{\"format\":\"assetdb\",\"version\":2}"};
    std::vector<std::string> richLines{"{\"format\":\"assetdb\",\"version\":2}"};
    for (int i = 0; i < kRecords; ++i)
    {
        plainLines.push_back(RecordLine(i, false));
        richLines.push_back(RecordLine(i, true));
    }
    WriteLines(root.Path() / "plain.assetdb", plainLines);
    WriteLines(root.Path() / "rich.assetdb", richLines);

    const std::uint64_t plain = CountLoadAllocations(root.Path() / "plain.assetdb");
    const std::uint64_t rich = CountLoadAllocations(root.Path() / "rich.assetdb");
    constexpr std::uint64_t kAllocationsPerKvMember = 4;
    RecordProperty("plainAllocations", std::to_string(plain));
    RecordProperty("richAllocations", std::to_string(rich));
    ASSERT_GT(rich, plain);
    EXPECT_LE(rich - plain, kAllocationsPerKvMember * kKvMembers * kRecords)
        << "plain " << plain << ", rich " << rich;
}
