// Links the real application objects, replacing only src/main.cpp.
// Exercises share-link parsing and serialization without startup, a database, or network access.
#include <QCoreApplication>
#include <QUrl>

#include <cstdio>
#include <memory>

#include "include/configs/outbounds/http.h"
#include "include/configs/outbounds/socks.h"

namespace {
    int checks = 0;
    int failures = 0;

    void check(bool condition, const QString& label) {
        ++checks;
        if (!condition) {
            ++failures;
            std::fprintf(stderr, "FAIL %s\n", label.toUtf8().constData());
        }
    }

    struct Auth {
        QString username;
        QString password;
    };

    bool isHttp(const QString& scheme) { return scheme == "http" || scheme == "https"; }

    std::unique_ptr<Configs::outbound> makeOutbound(const QString& scheme) {
        if (isHttp(scheme)) {
            auto out = std::make_unique<Configs::http>();
            out->tls->enabled = scheme == "https";
            return out;
        }
        auto out = std::make_unique<Configs::socks>();
        out->version = scheme.startsWith("socks4") ? 4 : 5;
        out->socks4a = scheme == "socks4a";
        return out;
    }

    void setAuth(Configs::outbound& out, const Auth& auth) {
        if (auto* http = dynamic_cast<Configs::http*>(&out)) {
            http->username = auth.username;
            http->password = auth.password;
        } else {
            auto& socks = dynamic_cast<Configs::socks&>(out);
            socks.username = auth.username;
            socks.password = auth.password;
        }
    }

    Auth getAuth(Configs::outbound& out) {
        if (auto* http = dynamic_cast<Configs::http*>(&out)) return {http->username, http->password};
        auto& socks = dynamic_cast<Configs::socks&>(out);
        return {socks.username, socks.password};
    }

    void checkAuth(const QString& scheme, const QString& userInfo, const Auth& expected, const QString& label) {
        auto parsed = makeOutbound(scheme);
        const auto link = scheme + "://" + userInfo + "127.0.0.1:1080#test";
        const auto prefix = scheme + ' ' + label;
        check(parsed->ParseFromLink(link), prefix + " parses");
        const auto actual = getAuth(*parsed);
        check(actual.username == expected.username, prefix + " username");
        check(actual.password == expected.password, prefix + " password");
        check(parsed->server == "127.0.0.1" && parsed->server_port == 1080 && parsed->name == "test", prefix + " endpoint and name");
    }

    void checkBase64(const QString& scheme, const Auth& auth, bool padding, bool percentEncoded, const QString& label) {
        auto encoded = (auth.username + ':' + auth.password).toUtf8().toBase64();
        if (!padding) {
            while (encoded.endsWith('=')) encoded.chop(1);
        }
        if (percentEncoded) encoded = QUrl::toPercentEncoding(QString::fromLatin1(encoded));
        auto expected = auth;
        if (isHttp(scheme) && expected.username.isEmpty() && !expected.password.isEmpty()) expected.username = expected.password;
        checkAuth(scheme, QString::fromLatin1(encoded) + '@', expected, label);
    }

    void checkRoundTrip(const QString& scheme, const Auth& auth, const QString& label) {
        auto original = makeOutbound(scheme);
        original->server = "127.0.0.1";
        original->server_port = 1080;
        original->name = QString::fromUtf8("Local fixture / 测试");
        setAuth(*original, auth);
        const QString link = original->ExportToLink();
        auto parsed = makeOutbound(scheme);
        const auto prefix = scheme + " round trip " + label;
        check(parsed->ParseFromLink(link), prefix + " parses");
        const auto actual = getAuth(*parsed);
        check(actual.username == auth.username, prefix + " username");
        check(actual.password == auth.password, prefix + " password");
        check(parsed->server == original->server && parsed->server_port == original->server_port && parsed->name == original->name,
              prefix + " endpoint and name");
        if (!auth.username.isEmpty() && auth.password.isEmpty()) {
            check(QUrl(link).userInfo(QUrl::FullyEncoded).endsWith(':'), prefix + " explicitly empty password");
        }
        if (isHttp(scheme)) {
            check(dynamic_cast<Configs::http&>(*parsed).tls->enabled == (scheme == "https"), prefix + " TLS");
        } else {
            const auto& parsedSocks = dynamic_cast<Configs::socks&>(*parsed);
            const auto& originalSocks = dynamic_cast<Configs::socks&>(*original);
            check(parsedSocks.version == originalSocks.version && parsedSocks.socks4a == originalSocks.socks4a, prefix + " SOCKS version");
        }
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    for (const auto& scheme : {QStringLiteral("socks"), QStringLiteral("socks4"), QStringLiteral("socks4a"),
                               QStringLiteral("socks5"), QStringLiteral("http"), QStringLiteral("https")}) {
        checkAuth(scheme, "user@", {"user", {}}, "ordinary username");
        checkAuth(scheme, "user:@", {"user", {}}, "explicit empty password");
        checkAuth(scheme, "dXNlcg==@", {"dXNlcg==", {}}, "base64 without delimiter");
        checkAuth(scheme, "dXNlcjpwYXNz:@", {"dXNlcjpwYXNz", {}}, "explicit empty password with base64-looking username");
        checkAuth(scheme, "dXNlcjpwYXNz:literal@", {"dXNlcjpwYXNz", "literal"}, "explicit nonempty password");
        checkAuth(scheme, "user%3Aname@", {"user:name", {}}, "encoded username colon");
        checkAuth(scheme, "u%2Fse%2Br%40name:p%3Aa%2Fs%2Bs%40%25@", {"u/se+r@name", "p:a/s+s@%"}, "encoded credential delimiters");
        checkAuth(scheme, "user%253Aname:p%252Fass@", {"user%3Aname", "p%2Fass"}, "decode percent escapes once");
        checkAuth(scheme, "", {}, "no credentials");
        checkAuth(scheme, "user:pass:with:colons@", {"user", "pass:with:colons"}, "password colons");
        checkAuth(scheme, ":token@", {isHttp(scheme) ? "token" : QString(), "token"}, "password-only compatibility");
        checkBase64(scheme, {"user", "pass"}, true, false, "base64 credentials");
        checkBase64(scheme, {"user", "pass"}, false, false, "unpadded base64 credentials");
        checkBase64(scheme, {"user", {}}, true, false, "base64 empty password");
        checkBase64(scheme, {{}, "token"}, true, false, "base64 empty username");
        checkBase64(scheme, {"user", "pass:with:colons"}, true, false, "base64 password colons");
        checkBase64(scheme, {"user", ">>>???"}, true, true, "percent-encoded base64");
        checkBase64(scheme, {QString::fromUtf8("用户"), QString::fromUtf8("пароль")}, true, true, "base64 UTF-8 credentials");
        checkRoundTrip(scheme, {"user", {}}, "null password");
        checkRoundTrip(scheme, {"user", QStringLiteral("")}, "empty password");
        checkRoundTrip(scheme, {"dXNlcjpwYXNz", {}}, "ambiguous username");
        checkRoundTrip(scheme, {"u:ser/@+%", {}}, "username delimiters");
        checkRoundTrip(scheme, {"u:ser/@+%", "p:a/ss@+%"}, "credential delimiters");
        checkRoundTrip(scheme, {QString::fromUtf8("用户"), QString::fromUtf8("пароль")}, "UTF-8 credentials");
        checkRoundTrip(scheme, {}, "no credentials");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
