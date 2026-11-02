#include "../mods/friend_unit_gift_server/sql_policy.h"

#include <codecvt>
#include <fstream>
#include <iostream>
#include <locale>
#include <string>

using namespace sdgo::friend_unit_gift;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: friend_unit_gift_sql_emit <database> <output>\n";
        return 2;
    }
    std::wstring database;
    for (const unsigned char value : std::string(argv[1])) {
        database.push_back(static_cast<wchar_t>(value));
    }
    auto policy = kManagerDefaults;
    policy.enabled = true;
    const auto sql = MakeInstallSql(database, database, policy);
    if (!sql) return 3;
    std::wofstream output(argv[2], std::ios::binary);
    output.imbue(std::locale(
        output.getloc(), new std::codecvt_utf8_utf16<wchar_t>));
    output << *sql;
    if (!output) return 4;
    return 0;
}
