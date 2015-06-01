/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2013-2015 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_LOGGING_AGENT_ADMIN_SERVER_H_
#define _PASSENGER_LOGGING_AGENT_ADMIN_SERVER_H_

#include <sstream>
#include <string>

#include <agents/LoggingAgent/LoggingServer.h>
#include <agents/AdminServerUtils.h>
#include <ApplicationPool2/ApiKey.h>
#include <ServerKit/HttpServer.h>
#include <DataStructures/LString.h>
#include <Exceptions.h>
#include <StaticString.h>
#include <Utils/StrIntUtils.h>
#include <Utils/modp_b64.h>
#include <Utils/json.h>
#include <Utils/BufferedIO.h>
#include <Utils/MessageIO.h>

namespace Passenger {
namespace LoggingAgent {

using namespace std;


class Request: public ServerKit::BaseHttpRequest {
public:
	string body;
	Json::Value jsonBody;

	DEFINE_SERVER_KIT_BASE_HTTP_REQUEST_FOOTER(Request);
};

class AdminServer: public ServerKit::HttpServer<AdminServer, ServerKit::HttpClient<Request> > {
private:
	typedef ServerKit::HttpServer<AdminServer, ServerKit::HttpClient<Request> > ParentClass;
	typedef ServerKit::HttpClient<Request> Client;
	typedef ServerKit::HeaderTable HeaderTable;

	void processPing(Client *client, Request *req) {
		if (authorizeStateInspectionOperation(this, client, req)) {
			HeaderTable headers;
			headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");
			headers.insert(req->pool, "Content-Type", "application/json");
			writeSimpleResponse(client, 200, &headers, "{ \"status\": \"ok\" }");
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith401(client, req);
		}
	}

	void processShutdown(Client *client, Request *req) {
		if (req->method != HTTP_POST) {
			respondWith405(client, req);
		} else if (authorizeAdminOperation(this, client, req)) {
			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "application/json");
			exitEvent->notify();
			writeSimpleResponse(client, 200, &headers, "{ \"status\": \"ok\" }");
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith401(client, req);
		}
	}

	void processConfig(Client *client, Request *req) {
		if (req->method == HTTP_GET) {
			if (!authorizeStateInspectionOperation(this, client, req)) {
				respondWith401(client, req);
			}

			HeaderTable headers;
			Json::Value doc;
			string logFile = getLogFile();
			string fileDescriptorLogFile = getFileDescriptorLogFile();

			headers.insert(req->pool, "Content-Type", "application/json");
			doc["log_level"] = getLogLevel();
			if (!logFile.empty()) {
				doc["log_file"] = logFile;
			}
			if (!fileDescriptorLogFile.empty()) {
				doc["file_descriptor_log_file"] = fileDescriptorLogFile;
			}

			writeSimpleResponse(client, 200, &headers, doc.toStyledString());
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else if (req->method == HTTP_PUT) {
			if (!authorizeAdminOperation(this, client, req)) {
				respondWith401(client, req);
			} else if (!req->hasBody()) {
				endAsBadRequest(&client, &req, "Body required");
			}
			// Continue in processConfigBody()
		} else {
			respondWith405(client, req);
		}
	}

	void processConfigBody(Client *client, Request *req) {
		HeaderTable headers;
		Json::Value &json = req->jsonBody;

		headers.insert(req->pool, "Content-Type", "application/json");

		if (json.isMember("log_level")) {
			setLogLevel(json["log_level"].asInt());
		}
		if (json.isMember("log_file")) {
			string logFile = json["log_file"].asString();
			try {
				logFile = absolutizePath(logFile);
			} catch (const SystemException &e) {
				unsigned int bufsize = 1024;
				char *message = (char *) psg_pnalloc(req->pool, bufsize);
				snprintf(message, bufsize, "{ \"status\": \"error\", "
					"\"message\": \"Cannot absolutize log file filename: %s\" }",
					e.what());
				writeSimpleResponse(client, 500, &headers, message);
				if (!req->ended()) {
					endRequest(&client, &req);
				}
				return;
			}

			int e;
			if (!setLogFile(logFile, &e)) {
				unsigned int bufsize = 1024;
				char *message = (char *) psg_pnalloc(req->pool, bufsize);
				snprintf(message, bufsize, "{ \"status\": \"error\", "
					"\"message\": \"Cannot open log file: %s (errno=%d)\" }",
					strerror(e), e);
				writeSimpleResponse(client, 500, &headers, message);
				if (!req->ended()) {
					endRequest(&client, &req);
				}
				return;
			}
			P_NOTICE("Log file opened.");
		}

		writeSimpleResponse(client, 200, &headers, "{ \"status\": \"ok\" }\n");
		if (!req->ended()) {
			endRequest(&client, &req);
		}
	}

	void processReinheritLogs(Client *client, Request *req) {
		if (req->method != HTTP_POST) {
			respondWith405(client, req);
		} else if (authorizeAdminOperation(this, client, req)) {
			HeaderTable headers;
			headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");
			headers.insert(req->pool, "Content-Type", "application/json");

			if (instanceDir.empty() || fdPassingPassword.empty()) {
				writeSimpleResponse(client, 501, &headers, "{ \"status\": \"error\", "
					"\"code\": \"NO_WATCHDOG\", "
					"\"message\": \"No Watchdog process\" }\n");
				if (!req->ended()) {
					endRequest(&client, &req);
				}
				return;
			}

			FileDescriptor watchdog(connectToUnixServer(instanceDir + "/agents.s/watchdog",
				NULL, 0), __FILE__, __LINE__);
			writeExact(watchdog,
				"GET /config/log_file.fd HTTP/1.1\r\n"
				"Connection: close\r\n"
				"Fd-Passing-Password: " + fdPassingPassword + "\r\n"
				"\r\n");
			BufferedIO io(watchdog);
			string response = io.readLine();
			SKC_DEBUG(client, "Watchdog response: \"" << cEscapeString(response) << "\"");
			if (response != "HTTP/1.1 200 OK\r\n") {
				watchdog.close();
				writeSimpleResponse(client, 500, &headers, "{ \"status\": \"error\", "
					"\"code\": \"INHERIT_ERROR\", "
					"\"message\": \"Error communicating with Watchdog process: non-200 response\" }\n");
				if (!req->ended()) {
					endRequest(&client, &req);
				}
				return;
			}

			string logFilePath;
			while (true) {
				response = io.readLine();
				SKC_DEBUG(client, "Watchdog response: \"" << cEscapeString(response) << "\"");
				if (response.empty()) {
					watchdog.close();
					writeSimpleResponse(client, 500, &headers, "{ \"status\": \"error\", "
						"\"code\": \"INHERIT_ERROR\", "
						"\"message\": \"Error communicating with Watchdog process: "
							"premature EOF encountered in response\" }\n");
					if (!req->ended()) {
						endRequest(&client, &req);
					}
					return;
				} else if (response == "\r\n") {
					break;
				} else if (startsWith(response, "filename: ")
					|| startsWith(response, "Filename: "))
				{
					response.erase(0, strlen("filename: "));
					logFilePath = response;
				}
			}

			if (logFilePath.empty()) {
				watchdog.close();
				writeSimpleResponse(client, 500, &headers, "{ \"status\": \"error\", "
					"\"code\": \"INHERIT_ERROR\", "
					"\"message\": \"Error communicating with Watchdog process: "
						"no log filename received in response\" }\n");
				if (!req->ended()) {
					endRequest(&client, &req);
				}
				return;
			}

			unsigned long long timeout = 1000000;
			int fd = readFileDescriptorWithNegotiation(watchdog, &timeout);
			setLogFileWithFd(logFilePath, fd);
			safelyClose(fd);
			watchdog.close();

			writeSimpleResponse(client, 200, &headers, "{ \"status\": \"ok\" }\n");
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith401(client, req);
		}
	}

	void processReopenLogs(Client *client, Request *req) {
		if (req->method != HTTP_POST) {
			respondWith405(client, req);
		} else if (authorizeAdminOperation(this, client, req)) {
			int e;
			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "application/json");

			string logFile = getLogFile();
			if (logFile.empty()) {
				writeSimpleResponse(client, 500, &headers, "{ \"status\": \"error\", "
					"\"code\": \"NO_LOG_FILE\", "
					"\"message\": \"" PROGRAM_NAME " was not configured with a log file.\" }\n");
				if (!req->ended()) {
					endRequest(&client, &req);
				}
				return;
			}

			if (!setLogFile(logFile, &e)) {
				unsigned int bufsize = 1024;
				char *message = (char *) psg_pnalloc(req->pool, bufsize);
				snprintf(message, bufsize, "{ \"status\": \"error\", "
					"\"code\": \"LOG_FILE_OPEN_ERROR\", "
					"\"message\": \"Cannot reopen log file %s: %s (errno=%d)\" }",
					logFile.c_str(), strerror(e), e);
				writeSimpleResponse(client, 500, &headers, message);
				if (!req->ended()) {
					endRequest(&client, &req);
				}
				return;
			}
			P_NOTICE("Log file reopened.");

			if (hasFileDescriptorLogFile()) {
				if (!setFileDescriptorLogFile(getFileDescriptorLogFile(), &e)) {
					unsigned int bufsize = 1024;
					char *message = (char *) psg_pnalloc(req->pool, bufsize);
					snprintf(message, bufsize, "{ \"status\": \"error\", "
						"\"code\": \"FD_LOG_FILE_OPEN_ERROR\", "
						"\"message\": \"Cannot reopen file descriptor log file %s: %s (errno=%d)\" }",
						getFileDescriptorLogFile().c_str(), strerror(e), e);
					writeSimpleResponse(client, 500, &headers, message);
					if (!req->ended()) {
						endRequest(&client, &req);
					}
					return;
				}
				P_NOTICE("File descriptor log file reopened.");
			}

			writeSimpleResponse(client, 200, &headers, "{ \"status\": \"ok\" }\n");

			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith401(client, req);
		}
	}

	void processStatusTxt(Client *client, Request *req) {
		if (req->method != HTTP_GET) {
			respondWith405(client, req);
		} else if (authorizeStateInspectionOperation(this, client, req)) {
			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "text/plain");

			stringstream stream;
			loggingServer->dump(stream);
			writeSimpleResponse(client, 200, &headers, stream.str());
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith401(client, req);
		}
	}

	void respondWith401(Client *client, Request *req) {
		HeaderTable headers;
		headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");
		headers.insert(req->pool, "WWW-Authenticate", "Basic realm=\"admin\"");
		writeSimpleResponse(client, 401, &headers, "Unauthorized");
		if (!req->ended()) {
			endRequest(&client, &req);
		}
	}

	void respondWith404(Client *client, Request *req) {
		HeaderTable headers;
		headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");
		writeSimpleResponse(client, 404, &headers, "Not found");
		if (!req->ended()) {
			endRequest(&client, &req);
		}
	}

	void respondWith405(Client *client, Request *req) {
		HeaderTable headers;
		headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");
		writeSimpleResponse(client, 405, &headers, "Method not allowed");
		if (!req->ended()) {
			endRequest(&client, &req);
		}
	}

	void respondWith422(Client *client, Request *req, const StaticString &body) {
		HeaderTable headers;
		headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");
		headers.insert(req->pool, "Content-Type", "text/plain; charset=utf-8");
		writeSimpleResponse(client, 422, &headers, body);
		if (!req->ended()) {
			endRequest(&client, &req);
		}
	}

protected:
	virtual void onRequestBegin(Client *client, Request *req) {
		const StaticString path(req->path.start->data, req->path.size);

		P_INFO("Admin request: " << http_method_str(req->method) <<
			" " << StaticString(req->path.start->data, req->path.size));

		try {
			if (path == P_STATIC_STRING("/ping.json")) {
				processPing(client, req);
			} else if (path == P_STATIC_STRING("/shutdown.json")) {
				processShutdown(client, req);
			} else if (path == P_STATIC_STRING("/config.json")) {
				processConfig(client, req);
			} else if (path == P_STATIC_STRING("/reinherit_logs.json")) {
				processReinheritLogs(client, req);
			} else if (path == P_STATIC_STRING("/reopen_logs.json")) {
				processReopenLogs(client, req);
			} else if (path == P_STATIC_STRING("/status.txt")) {
				processStatusTxt(client, req);
			} else {
				respondWith404(client, req);
			}
		} catch (const oxt::tracable_exception &e) {
			SKC_ERROR(client, "Exception: " << e.what() << "\n" << e.backtrace());
			if (!req->ended()) {
				req->wantKeepAlive = false;
				endRequest(&client, &req);
			}
		}
	}

	virtual ServerKit::Channel::Result onRequestBody(Client *client, Request *req,
		const MemoryKit::mbuf &buffer, int errcode)
	{
		if (buffer.size() > 0) {
			// Data
			req->body.append(buffer.start, buffer.size());
		} else if (errcode == 0) {
			// EOF
			Json::Reader reader;
			if (reader.parse(req->body, req->jsonBody)) {
				processConfigBody(client, req);
			} else {
				respondWith422(client, req, reader.getFormattedErrorMessages());
			}
		} else {
			// Error
			disconnect(&client);
		}
		return ServerKit::Channel::Result(buffer.size(), false);
	}

	virtual void deinitializeRequest(Client *client, Request *req) {
		req->body.clear();
		if (!req->jsonBody.isNull()) {
			req->jsonBody = Json::Value();
		}
		ParentClass::deinitializeRequest(client, req);
	}

public:
	LoggingServer *loggingServer;
	AdminAccountDatabase *adminAccountDatabase;
	string instanceDir;
	string fdPassingPassword;
	EventFd *exitEvent;

	AdminServer(ServerKit::Context *context)
		: ParentClass(context),
		  loggingServer(NULL),
		  adminAccountDatabase(NULL),
		  exitEvent(NULL)
		{ }

	virtual StaticString getServerName() const {
		return P_STATIC_STRING("LoggerAdminServer");
	}

	virtual unsigned int getClientName(const Client *client, char *buf, size_t size) const {
		return ParentClass::getClientName(client, buf, size);
	}

	bool authorizeByUid(uid_t uid) const {
		return uid == 0 || uid == geteuid();
	}

	bool authorizeByApiKey(const ApplicationPool2::ApiKey &apiKey) const {
		return apiKey.isSuper();
	}
};


} // namespace LoggingAgent
} // namespace Passenger

#endif /* _PASSENGER_LOGGING_AGENT_ADMIN_SERVER_H_ */
