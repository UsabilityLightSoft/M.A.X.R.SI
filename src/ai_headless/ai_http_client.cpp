/***************************************************************************
 * ai_http_client.cpp — минимальный HTTP POST к localhost (этап 5).
 *
 * Неблокирующий connect + select с общим дедлайном на всю операцию;
 * ответ читается до закрытия соединения или до Content-Length,
 * поддержан Transfer-Encoding: chunked.
 ***************************************************************************/

#include "ai_http_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <arpa/inet.h>
# include <cerrno>
# include <fcntl.h>
# include <netdb.h>
# include <netinet/in.h>
# include <sys/select.h>
# include <sys/socket.h>
# include <sys/types.h>
# include <unistd.h>
#endif

namespace ai
{

namespace
{

// --- платформенная прослойка -------------------------------------------------

#ifdef _WIN32
using tSocket = SOCKET;
using tSockLen = int;
const tSocket kNoSocket = INVALID_SOCKET;
void closeSocket (tSocket s) { closesocket (s); }
int lastSocketError() { return WSAGetLastError(); }
bool wouldBlock (int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
bool interrupted (int e) { return e == WSAEINTR; }
#else
using tSocket = int;
using tSockLen = socklen_t;
const tSocket kNoSocket = -1;
void closeSocket (tSocket s) { ::close (s); }
int lastSocketError() { return errno; }
bool wouldBlock (int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS; }
bool interrupted (int e) { return e == EINTR; }
#endif

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL; // без SIGPIPE на закрытом сокете
#else
constexpr int kSendFlags = 0;
#endif

constexpr std::size_t kMaxResponse = 16u * 1024u * 1024u; // предел ответа: 16 МБ

using tClock = std::chrono::steady_clock;

std::string errorText (int e)
{
#ifdef _WIN32
	switch (e)
	{
		case WSAECONNREFUSED: return "соединение отклонено — порт никто не слушает (WSA 10061)";
		case WSAETIMEDOUT: return "таймаут соединения (WSA 10060)";
		case WSAECONNRESET: return "соединение сброшено сервером (WSA 10054)";
		case WSAECONNABORTED: return "соединение прервано (WSA 10053)";
		case WSAENETUNREACH: return "сеть недоступна (WSA 10051)";
		case WSAEHOSTUNREACH: return "узел недоступен (WSA 10065)";
		case WSAEADDRNOTAVAIL: return "адрес недоступен (WSA 10049)";
		default: return "ошибка сокета WSA " + std::to_string (e);
	}
#else
	return std::string (std::strerror (e)) + " (errno " + std::to_string (e) + ")";
#endif
}

bool ensureNetInit (std::string& err)
{
#ifdef _WIN32
	// WSAStartup — один раз на процесс (потокобезопасная статическая инициализация)
	static const int rc = [] {
		WSADATA data;
		return WSAStartup (MAKEWORD (2, 2), &data);
	}();
	if (rc != 0)
	{
		err = "WSAStartup: код " + std::to_string (rc);
		return false;
	}
#else
	(void) err;
#endif
	return true;
}

struct sSocketGuard
{
	tSocket s = kNoSocket;
	~sSocketGuard()
	{
		if (s != kNoSocket) closeSocket (s);
	}
};

struct sAddrInfoGuard
{
	addrinfo* p = nullptr;
	~sAddrInfoGuard()
	{
		if (p != nullptr) freeaddrinfo (p);
	}
};

bool setNonBlocking (tSocket s)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket (s, FIONBIO, &mode) == 0;
#else
	const int flags = fcntl (s, F_GETFL, 0);
	return flags >= 0 && fcntl (s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

long remainingMs (tClock::time_point deadline)
{
	return static_cast<long> (std::chrono::duration_cast<std::chrono::milliseconds> (deadline - tClock::now()).count());
}

enum class eWait
{
	Ready,
	Timeout,
	Error
};

// ждать готовности сокета на чтение/запись до дедлайна; failed — сокет
// в exceptfds (так Windows сообщает о неудачном неблокирующем connect)
eWait waitSocket (tSocket s, bool forWrite, tClock::time_point deadline, bool& failed)
{
	failed = false;
	for (;;)
	{
		const long ms = remainingMs (deadline);
		if (ms <= 0) return eWait::Timeout;
		fd_set ioSet;
		fd_set exSet;
		FD_ZERO (&ioSet);
		FD_ZERO (&exSet);
		FD_SET (s, &ioSet);
		FD_SET (s, &exSet);
		timeval tv;
		tv.tv_sec = ms / 1000;
		tv.tv_usec = (ms % 1000) * 1000;
		const int n = select (static_cast<int> (s) + 1, forWrite ? nullptr : &ioSet, forWrite ? &ioSet : nullptr, &exSet, &tv);
		if (n > 0)
		{
			failed = FD_ISSET (s, &exSet) != 0;
			return eWait::Ready;
		}
		if (n == 0) continue; // дедлайн проверится в начале цикла
		if (interrupted (lastSocketError())) continue;
		return eWait::Error;
	}
}

// --- URL ---------------------------------------------------------------------

struct sUrl
{
	std::string host;
	int port = 80;
	std::string path = "/";
	bool ipv6 = false;
};

std::string toLower (std::string s)
{
	std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
	return s;
}

std::string trim (const std::string& s)
{
	const auto b = s.find_first_not_of (" \t");
	if (b == std::string::npos) return {};
	const auto e = s.find_last_not_of (" \t");
	return s.substr (b, e - b + 1);
}

bool parseUrl (const std::string& url, sUrl& out, std::string& err)
{
	const std::string lower = toLower (url);
	if (lower.rfind ("https://", 0) == 0)
	{
		err = "https не поддерживается — только http://";
		return false;
	}
	if (lower.rfind ("http://", 0) != 0)
	{
		err = "только схема http://: " + url;
		return false;
	}
	const std::string rest = url.substr (7);
	const auto slash = rest.find ('/');
	const std::string authority = rest.substr (0, slash);
	out.path = (slash == std::string::npos) ? "/" : rest.substr (slash);
	for (unsigned char c : out.path)
	{
		if (c <= 0x20 || c == 0x7f)
		{
			err = "недопустимый символ в пути URL";
			return false;
		}
	}
	if (authority.find ('@') != std::string::npos)
	{
		err = "логин в URL не поддерживается";
		return false;
	}

	std::string portText;
	if (!authority.empty() && authority[0] == '[')
	{
		const auto close = authority.find (']');
		if (close == std::string::npos)
		{
			err = "нет ']' в IPv6-адресе";
			return false;
		}
		out.host = authority.substr (1, close - 1);
		out.ipv6 = true;
		const std::string after = authority.substr (close + 1);
		if (!after.empty())
		{
			if (after[0] != ':')
			{
				err = "мусор после IPv6-адреса";
				return false;
			}
			portText = after.substr (1);
		}
	}
	else
	{
		const auto colon = authority.find (':');
		if (colon != std::string::npos && authority.find (':', colon + 1) != std::string::npos)
		{
			err = "IPv6-адрес пишется в квадратных скобках: http://[::1]:порт/";
			return false;
		}
		out.host = authority.substr (0, colon);
		if (colon != std::string::npos) portText = authority.substr (colon + 1);
	}

	if (!portText.empty())
	{
		if (portText.size() > 5 || !std::all_of (portText.begin(), portText.end(), [] (unsigned char c) { return std::isdigit (c) != 0; }))
		{
			err = "неверный порт: " + portText;
			return false;
		}
		out.port = std::stoi (portText);
		if (out.port < 1 || out.port > 65535)
		{
			err = "порт вне 1..65535: " + portText;
			return false;
		}
	}

	out.host = toLower (out.host);
	if (out.host != "127.0.0.1" && out.host != "localhost" && out.host != "::1")
	{
		err = "только localhost (127.0.0.1, localhost, ::1), а не «" + out.host + "»";
		return false;
	}
	return true;
}

// адрес — петлевой (127.0.0.0/8 или ::1): страховка, что getaddrinfo не увёл наружу
bool isLoopback (const sockaddr* a)
{
	if (a->sa_family == AF_INET)
	{
		const auto* in = reinterpret_cast<const sockaddr_in*> (a);
		return (ntohl (in->sin_addr.s_addr) >> 24) == 127;
	}
	if (a->sa_family == AF_INET6)
	{
		const auto* in6 = reinterpret_cast<const sockaddr_in6*> (a);
		const auto* b = reinterpret_cast<const unsigned char*> (&in6->sin6_addr);
		for (int i = 0; i < 15; ++i)
			if (b[i] != 0) return false;
		return b[15] == 1;
	}
	return false;
}

// --- соединение, отправка, приём --------------------------------------------

bool connectWithDeadline (tSocket s, const addrinfo& ai, tClock::time_point deadline, std::string& err)
{
	if (::connect (s, ai.ai_addr, static_cast<tSockLen> (ai.ai_addrlen)) == 0) return true;
	const int e = lastSocketError();
	if (!wouldBlock (e))
	{
		err = "connect: " + errorText (e);
		return false;
	}
	bool failed = false;
	const eWait w = waitSocket (s, true, deadline, failed);
	if (w == eWait::Timeout)
	{
		err = "connect: таймаут";
		return false;
	}
	if (w == eWait::Error)
	{
		err = "connect/select: " + errorText (lastSocketError());
		return false;
	}
	int soErr = 0;
	tSockLen len = sizeof (soErr);
	if (getsockopt (s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*> (&soErr), &len) != 0)
		soErr = lastSocketError();
	if (soErr != 0 || failed)
	{
		err = "connect: " + errorText (soErr);
		return false;
	}
	return true;
}

bool sendAll (tSocket s, const std::string& data, tClock::time_point deadline, std::string& err)
{
	std::size_t sent = 0;
	while (sent < data.size())
	{
		bool failed = false;
		const eWait w = waitSocket (s, true, deadline, failed);
		if (w == eWait::Timeout)
		{
			err = "send: таймаут";
			return false;
		}
		if (w == eWait::Error || failed)
		{
			err = "send/select: " + errorText (lastSocketError());
			return false;
		}
		const int chunk = static_cast<int> (std::min<std::size_t> (data.size() - sent, 1u << 20));
		const auto n = ::send (s, data.data() + sent, chunk, kSendFlags);
		if (n < 0)
		{
			const int e = lastSocketError();
			if (wouldBlock (e) || interrupted (e)) continue;
			err = "send: " + errorText (e);
			return false;
		}
		sent += static_cast<std::size_t> (n);
	}
	return true;
}

int hexDigit (char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// собрать chunked-тело начиная с pos; complete = false — данных пока не хватает
bool decodeChunked (const std::string& raw, std::size_t pos, std::string& out, bool& complete, std::string& err)
{
	out.clear();
	complete = false;
	for (;;)
	{
		const auto lineEnd = raw.find ("\r\n", pos);
		if (lineEnd == std::string::npos) return true;
		std::string sizeText = raw.substr (pos, lineEnd - pos);
		const auto semi = sizeText.find (';'); // расширения блока игнорируем
		if (semi != std::string::npos) sizeText.resize (semi);
		sizeText = trim (sizeText);
		if (sizeText.empty())
		{
			err = "chunked: пустой размер блока";
			return false;
		}
		std::size_t size = 0;
		for (char c : sizeText)
		{
			const int d = hexDigit (c);
			if (d < 0)
			{
				err = "chunked: неверный размер блока «" + sizeText + "»";
				return false;
			}
			size = size * 16 + static_cast<std::size_t> (d);
			if (size > kMaxResponse)
			{
				err = "chunked: блок больше предела";
				return false;
			}
		}
		pos = lineEnd + 2;
		if (size == 0)
		{
			// последний блок: трейлеры до пустой строки
			for (;;)
			{
				const auto e = raw.find ("\r\n", pos);
				if (e == std::string::npos) return true;
				if (e == pos)
				{
					complete = true;
					return true;
				}
				pos = e + 2;
			}
		}
		if (raw.size() < pos + size + 2) return true;
		out.append (raw, pos, size);
		pos += size;
		if (raw.compare (pos, 2, "\r\n") != 0)
		{
			err = "chunked: нет CRLF после блока";
			return false;
		}
		pos += 2;
	}
}

struct sResponseHead
{
	int status = 0;
	long long contentLength = -1;
	bool chunked = false;
};

bool parseHead (const std::string& head, sResponseHead& out, std::string& err)
{
	const auto firstEnd = head.find ("\r\n");
	const std::string statusLine = head.substr (0, firstEnd);
	if (statusLine.rfind ("HTTP/", 0) != 0)
	{
		err = "ответ не HTTP: «" + statusLine.substr (0, 60) + "»";
		return false;
	}
	const auto sp = statusLine.find (' ');
	if (sp == std::string::npos || sp + 4 > statusLine.size()
		|| !std::isdigit (static_cast<unsigned char> (statusLine[sp + 1]))
		|| !std::isdigit (static_cast<unsigned char> (statusLine[sp + 2]))
		|| !std::isdigit (static_cast<unsigned char> (statusLine[sp + 3])))
	{
		err = "неверная строка статуса: «" + statusLine.substr (0, 60) + "»";
		return false;
	}
	out.status = std::stoi (statusLine.substr (sp + 1, 3));

	std::size_t pos = (firstEnd == std::string::npos) ? head.size() : firstEnd + 2;
	while (pos < head.size())
	{
		auto end = head.find ("\r\n", pos);
		if (end == std::string::npos) end = head.size();
		const std::string line = head.substr (pos, end - pos);
		pos = end + 2;
		const auto colon = line.find (':');
		if (colon == std::string::npos) continue;
		const std::string name = toLower (trim (line.substr (0, colon)));
		const std::string value = trim (line.substr (colon + 1));
		if (name == "content-length")
		{
			if (value.empty() || value.size() > 12
				|| !std::all_of (value.begin(), value.end(), [] (unsigned char c) { return std::isdigit (c) != 0; }))
			{
				err = "неверный Content-Length: «" + value + "»";
				return false;
			}
			out.contentLength = std::stoll (value);
		}
		else if (name == "transfer-encoding")
		{
			if (toLower (value).find ("chunked") != std::string::npos) out.chunked = true;
		}
	}
	if (out.chunked) out.contentLength = -1; // chunked важнее Content-Length (RFC 7230)
	return true;
}

sHttpResult doPost (const std::string& url, const std::string& body, int timeoutMs, tClock::time_point start)
{
	sHttpResult r;
	if (timeoutMs <= 0)
	{
		r.error = "таймаут должен быть > 0 мс";
		return r;
	}
	const auto deadline = start + std::chrono::milliseconds (timeoutMs);

	sUrl u;
	if (!parseUrl (url, u, r.error)) return r;
	if (!ensureNetInit (r.error)) return r;

	// разрешение адреса (числовые — без DNS; localhost — локально)
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	if (u.host != "localhost") hints.ai_flags = AI_NUMERICHOST;
	sAddrInfoGuard addrs;
	const std::string portText = std::to_string (u.port);
	const int gai = getaddrinfo (u.host.c_str(), portText.c_str(), &hints, &addrs.p);
	if (gai != 0 || addrs.p == nullptr)
	{
		r.error = "getaddrinfo(" + u.host + "): код " + std::to_string (gai);
		return r;
	}

	// соединиться с первым петлевым адресом, который ответит. IPv4 — первым:
	// localhost даёт сперва ::1, а LM Studio слушает 127.0.0.1, отказ же на
	// loopback Windows сообщает только через ~2 с.
	std::vector<const addrinfo*> order;
	for (const addrinfo* ai = addrs.p; ai != nullptr; ai = ai->ai_next)
		if (ai->ai_family == AF_INET) order.push_back (ai);
	for (const addrinfo* ai = addrs.p; ai != nullptr; ai = ai->ai_next)
		if (ai->ai_family != AF_INET) order.push_back (ai);
	sSocketGuard sock;
	std::string connectErr = "нет петлевых адресов для " + u.host;
	for (const addrinfo* ai : order)
	{
		if (!isLoopback (ai->ai_addr)) continue;
		if (remainingMs (deadline) <= 0)
		{
			connectErr = "connect: таймаут";
			break;
		}
		tSocket s = ::socket (ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == kNoSocket)
		{
			connectErr = "socket: " + errorText (lastSocketError());
			continue;
		}
		sSocketGuard candidate;
		candidate.s = s;
#ifdef SO_NOSIGPIPE
		const int one = 1;
		setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
#endif
		if (!setNonBlocking (s))
		{
			connectErr = "неблокирующий режим: " + errorText (lastSocketError());
			continue;
		}
		if (connectWithDeadline (s, *ai, deadline, connectErr))
		{
			sock.s = candidate.s;
			candidate.s = kNoSocket;
			break;
		}
	}
	if (sock.s == kNoSocket)
	{
		r.error = connectErr;
		return r;
	}

	// запрос
	const std::string hostHeader = (u.ipv6 ? "[" + u.host + "]" : u.host) + ":" + portText;
	std::string request;
	request.reserve (body.size() + 256);
	request += "POST " + u.path + " HTTP/1.1\r\n";
	request += "Host: " + hostHeader + "\r\n";
	request += "Content-Type: application/json\r\n";
	request += "Accept: application/json\r\n";
	request += "Content-Length: " + std::to_string (body.size()) + "\r\n";
	request += "Connection: close\r\n\r\n";
	request += body;
	if (!sendAll (sock.s, request, deadline, r.error)) return r;

	// ответ: до закрытия, до Content-Length или до конца chunked
	std::string raw;
	std::size_t bodyStart = std::string::npos;
	sResponseHead head;
	std::string decoded;
	bool chunkedDone = false;
	char buf[64 * 1024];
	for (;;)
	{
		if (bodyStart != std::string::npos)
		{
			if (head.contentLength >= 0 && static_cast<long long> (raw.size() - bodyStart) >= head.contentLength) break;
			if (head.chunked)
			{
				if (!decodeChunked (raw, bodyStart, decoded, chunkedDone, r.error)) return r;
				if (chunkedDone) break;
			}
		}
		bool failed = false;
		const eWait w = waitSocket (sock.s, false, deadline, failed);
		if (w == eWait::Timeout)
		{
			r.error = raw.empty() ? "recv: таймаут — сервер не ответил" : "recv: таймаут — ответ не дочитан";
			return r;
		}
		if (w == eWait::Error)
		{
			r.error = "recv/select: " + errorText (lastSocketError());
			return r;
		}
		const auto n = ::recv (sock.s, buf, static_cast<int> (sizeof (buf)), 0);
		if (n == 0) break; // сервер закрыл соединение
		if (n < 0)
		{
			const int e = lastSocketError();
			if (wouldBlock (e) || interrupted (e)) continue;
			// сброс после полного ответа без Content-Length — считаем концом
			if (bodyStart != std::string::npos && head.contentLength < 0 && !head.chunked) break;
			r.error = "recv: " + errorText (e);
			return r;
		}
		raw.append (buf, static_cast<std::size_t> (n));
		if (raw.size() > kMaxResponse)
		{
			r.error = "ответ больше 16 МБ";
			return r;
		}
		if (bodyStart == std::string::npos)
		{
			const auto p = raw.find ("\r\n\r\n");
			if (p != std::string::npos)
			{
				if (!parseHead (raw.substr (0, p), head, r.error)) return r;
				bodyStart = p + 4;
			}
		}
	}

	if (bodyStart == std::string::npos)
	{
		r.error = raw.empty() ? "сервер закрыл соединение без ответа" : "оборванный ответ: нет конца заголовков";
		return r;
	}
	if (head.chunked)
	{
		if (!chunkedDone)
		{
			if (!decodeChunked (raw, bodyStart, decoded, chunkedDone, r.error)) return r;
			if (!chunkedDone)
			{
				r.error = "chunked: соединение закрыто до последнего блока";
				return r;
			}
		}
		r.body = std::move (decoded);
	}
	else if (head.contentLength >= 0)
	{
		if (static_cast<long long> (raw.size() - bodyStart) < head.contentLength)
		{
			r.error = "тело короче Content-Length (" + std::to_string (raw.size() - bodyStart) + " из "
				+ std::to_string (head.contentLength) + ")";
			return r;
		}
		r.body = raw.substr (bodyStart, static_cast<std::size_t> (head.contentLength));
	}
	else
		r.body = raw.substr (bodyStart);

	r.status = head.status;
	r.ok = true;
	return r;
}

} // namespace

sHttpResult httpPostJson (const std::string& url, const std::string& body, int timeoutMs)
{
	const auto start = tClock::now();
	sHttpResult r;
	try
	{
		r = doPost (url, body, timeoutMs, start);
	}
	catch (const std::exception& ex)
	{
		r = sHttpResult{};
		r.error = std::string ("исключение: ") + ex.what();
	}
	catch (...)
	{
		r = sHttpResult{};
		r.error = "неизвестное исключение";
	}
	r.elapsedMs = static_cast<long> (std::chrono::duration_cast<std::chrono::milliseconds> (tClock::now() - start).count());
	return r;
}

} // namespace ai
