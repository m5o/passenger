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
#ifndef _PASSENGER_SERVER_AGENT_ADMIN_SERVER_H_
#define _PASSENGER_SERVER_AGENT_ADMIN_SERVER_H_

#include <boost/regex.hpp>
#include <oxt/thread.hpp>
#include <sstream>
#include <string>
#include <cstring>
#include <sys/types.h>

#include <agents/HelperAgent/RequestHandler.h>
#include <agents/AdminServerUtils.h>
#include <ApplicationPool2/Pool.h>
#include <ServerKit/HttpServer.h>
#include <DataStructures/LString.h>
#include <Exceptions.h>
#include <StaticString.h>
#include <Logging.h>
#include <Constants.h>
#include <Utils/StrIntUtils.h>
#include <Utils/modp_b64.h>
#include <Utils/json.h>
#include <Utils/BufferedIO.h>
#include <Utils/MessageIO.h>

namespace Passenger {
namespace ServerAgent {

using namespace std;


class Request: public ServerKit::BaseHttpRequest {
public:
	string body;
	Json::Value jsonBody;
	Authorization authorization;

	DEFINE_SERVER_KIT_BASE_HTTP_REQUEST_FOOTER(Request);
};

class AdminServer: public ServerKit::HttpServer<AdminServer, ServerKit::HttpClient<Request> > {
private:
	typedef ServerKit::HttpServer<AdminServer, ServerKit::HttpClient<Request> > ParentClass;
	typedef ServerKit::HttpClient<Request> Client;
	typedef ServerKit::HeaderTable HeaderTable;

	boost::regex serverConnectionPath;

	bool regex_match(const StaticString &str, const boost::regex &e) const {
		return boost::regex_match(str.data(), str.data() + str.size(), e);
	}

	int extractThreadNumberFromClientName(const string &clientName) const {
		boost::smatch results;
		boost::regex re("^([0-9]+)-.*");

		if (!boost::regex_match(clientName, results, re)) {
			return -1;
		}
		if (results.size() != 2) {
			return -1;
		}
		return stringToUint(results.str(1));
	}

	static void disconnectClient(RequestHandler *rh, string clientName) {
		rh->disconnect(clientName);
	}

	void processServerConnectionOperation(Client *client, Request *req) {
		if (!authorizeAdminOperation(this, client, req)) {
			respondWith401(client, req);
		} else if (req->method == HTTP_DELETE) {
			StaticString path = req->getPathWithoutQueryString();
			boost::smatch results;

			boost::regex_match(path.toString(), results, serverConnectionPath);
			if (results.size() != 2) {
				endAsBadRequest(&client, &req, "Invalid URI");
				return;
			}

			int threadNumber = extractThreadNumberFromClientName(results.str(1));
			P_WARN(results.str(1));
			P_WARN(threadNumber);
			if (threadNumber < 1 || (unsigned int) threadNumber > requestHandlers.size()) {
				HeaderTable headers;
				headers.insert(req->pool, "Content-Type", "application/json");
				writeSimpleResponse(client, 400, &headers,
					"{ \"status\": \"error\", \"reason\": \"Invalid thread number\" }");
				if (!req->ended()) {
					endRequest(&client, &req);
				}
				return;
			}

			requestHandlers[threadNumber - 1]->getContext()->libev->runLater(boost::bind(
				disconnectClient, requestHandlers[threadNumber - 1], results.str(1)));

			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "application/json");
			writeSimpleResponse(client, 200, &headers,
				"{ \"status\": \"ok\" }");
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith405(client, req);
		}
	}

	static void inspectRequestHandlerState(RequestHandler *rh, Json::Value *json) {
		*json = rh->inspectStateAsJson();
	}

	void processServerStatus(Client *client, Request *req) {
		if (authorizeStateInspectionOperation(this, client, req)) {
			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "application/json");

			Json::Value doc;
			doc["threads"] = (Json::UInt) requestHandlers.size();
			for (unsigned int i = 0; i < requestHandlers.size(); i++) {
				Json::Value json;
				string key = "thread" + toString(i + 1);

				requestHandlers[i]->getContext()->libev->runSync(boost::bind(
					inspectRequestHandlerState, requestHandlers[i], &json));
				doc[key] = json;
			}

			writeSimpleResponse(client, 200, &headers,
				psg_pstrdup(req->pool, doc.toStyledString()));
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith401(client, req);
		}
	}

	void processPoolStatusXml(Client *client, Request *req) {
		Authorization auth(authorize(this, client, req));
		if (auth.canReadPool) {
			ApplicationPool2::Pool::ToXmlOptions options(
				parseQueryString(req->getQueryString()));
			options.uid = auth.uid;
			options.apiKey = auth.apiKey;

			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "text/xml");
			writeSimpleResponse(client, 200, &headers,
				psg_pstrdup(req->pool, appPool->toXml(options)));
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith401(client, req);
		}
	}

	void processPoolStatusTxt(Client *client, Request *req) {
		Authorization auth(authorize(this, client, req));
		if (auth.canReadPool) {
			ApplicationPool2::Pool::InspectOptions options(
				parseQueryString(req->getQueryString()));
			options.uid = auth.uid;
			options.apiKey = auth.apiKey;

			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "text/plain");
			writeSimpleResponse(client, 200, &headers,
				psg_pstrdup(req->pool, appPool->inspect(options)));
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith401(client, req);
		}
	}

	void processPoolRestartAppGroup(Client *client, Request *req) {
		Authorization auth(authorize(this, client, req));
		if (!auth.canModifyPool) {
			respondWith401(client, req);
		} else if (req->method != HTTP_POST) {
			respondWith405(client, req);
		} else if (!req->hasBody()) {
			endAsBadRequest(&client, &req, "Body required");
		} else if (requestBodyExceedsLimit(client, req)) {
			respondWith413(client, req);
		} else {
			req->authorization = auth;
			// Continues in processPoolRestartAppGroupBody().
		}
	}

	void processPoolRestartAppGroupBody(Client *client, Request *req) {
		if (!req->jsonBody.isMember("name")) {
			endAsBadRequest(&client, &req, "Name required");
			return;
		}

		ApplicationPool2::Pool::RestartOptions options;
		options.uid = req->authorization.uid;
		options.apiKey = req->authorization.apiKey;
		if (req->jsonBody.isMember("restart_method")) {
			string restartMethodString = req->jsonBody["restart_method"].asString();
			if (restartMethodString == "blocking") {
				options.method = RM_BLOCKING;
			} else if (restartMethodString == "rolling") {
				options.method = RM_ROLLING;
			} else {
				endAsBadRequest(&client, &req, "Unsupported restart method");
				return;
			}
		}

		bool result;
		const char *response;
		try {
			result = appPool->restartGroupByName(req->jsonBody["name"].asString(),
				options);
		} catch (const SecurityException &) {
			respondWith401(client, req);
			return;
		}
		if (result) {
			response = "{ \"restarted\": true }";
		} else {
			response = "{ \"restarted\": false }";
		}

		HeaderTable headers;
		headers.insert(req->pool, "Content-Type", "application/json");
		headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");
		writeSimpleResponse(client, 200, &headers, response);

		if (!req->ended()) {
			endRequest(&client, &req);
		}
	}

	void processPoolDetachProcess(Client *client, Request *req) {
		Authorization auth(authorize(this, client, req));
		if (!auth.canModifyPool) {
			respondWith401(client, req);
		} else if (req->method != HTTP_POST) {
			respondWith405(client, req);
		} else if (!req->hasBody()) {
			endAsBadRequest(&client, &req, "Body required");
		} else if (requestBodyExceedsLimit(client, req)) {
			respondWith413(client, req);
		} else {
			req->authorization = auth;
			// Continues in processPoolDetachProcessBody().
		}
	}

	void processPoolDetachProcessBody(Client *client, Request *req) {
		if (req->jsonBody.isMember("pid")) {
			pid_t pid = (pid_t) req->jsonBody["pid"].asUInt();
			ApplicationPool2::Pool::AuthenticationOptions options;
			options.uid = req->authorization.uid;
			options.apiKey = req->authorization.apiKey;

			bool result;
			try {
				result = appPool->detachProcess(pid, options);
			} catch (const SecurityException &) {
				respondWith401(client, req);
				return;
			}

			const char *response;
			if (result) {
				response = "{ \"detached\": true }";
			} else {
				response = "{ \"detached\": false }";
			}

			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "application/json");
			headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");
			writeSimpleResponse(client, 200, &headers, response);
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			endAsBadRequest(&client, &req, "PID required");
		}
	}

	void processBacktraces(Client *client, Request *req) {
		if (authorizeStateInspectionOperation(this, client, req)) {
			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "text/plain");
			writeSimpleResponse(client, 200, &headers,
				psg_pstrdup(req->pool, oxt::thread::all_backtraces()));
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith401(client, req);
		}
	}

	void processPing(Client *client, Request *req) {
		Authorization auth(authorize(this, client, req));
		if (auth.canReadPool || auth.canInspectState) {
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
		if (req->method != HTTP_PUT) {
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

	static void garbageCollect(RequestHandler *rh) {
		ServerKit::Context *ctx = rh->getContext();
		unsigned int count;

		count = mbuf_pool_compact(&ctx->mbuf_pool);
		SKS_NOTICE_FROM_STATIC(rh, "Freed " << count << " mbufs");

		rh->compact(LVL_NOTICE);
	}

	void processGc(Client *client, Request *req) {
		if (req->method != HTTP_PUT) {
			respondWith405(client, req);
		} else if (authorizeAdminOperation(this, client, req)) {
			HeaderTable headers;
			headers.insert(req->pool, "Content-Type", "application/json");
			for (unsigned int i = 0; i < requestHandlers.size(); i++) {
				requestHandlers[i]->getContext()->libev->runLater(boost::bind(
					garbageCollect, requestHandlers[i]));
			}
			writeSimpleResponse(client, 200, &headers, "{ \"status\": \"ok\" }");
			if (!req->ended()) {
				endRequest(&client, &req);
			}
		} else {
			respondWith401(client, req);
		}
	}

	static void getRequestHandlerConfig(RequestHandler *rh, Json::Value *json) {
		*json = rh->getConfigAsJson();
	}

	void processConfig(Client *client, Request *req) {
		if (req->method == HTTP_GET) {
			if (!authorizeStateInspectionOperation(this, client, req)) {
				respondWith401(client, req);
			}

			HeaderTable headers;
			string logFile = getLogFile();
			string fileDescriptorLogFile = getFileDescriptorLogFile();

			headers.insert(req->pool, "Content-Type", "application/json");
			Json::Value doc;
			requestHandlers[0]->getContext()->libev->runSync(boost::bind(
				getRequestHandlerConfig, requestHandlers[0], &doc));
			doc["log_level"] = getLogLevel();
			if (!logFile.empty()) {
				doc["log_file"] = logFile;
			}
			if (!fileDescriptorLogFile.empty()) {
				doc["file_descriptor_log_file"] = fileDescriptorLogFile;
			}

			writeSimpleResponse(client, 200, &headers,
				psg_pstrdup(req->pool, doc.toStyledString()));
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

	static void configureRequestHandler(RequestHandler *rh, Json::Value json) {
		rh->configure(json);
	}

	void processConfigBody(Client *client, Request *req) {
		HeaderTable headers;
		Json::Value &json = req->jsonBody;

		headers.insert(req->pool, "Content-Type", "application/json");
		headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");

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
		for (unsigned int i = 0; i < requestHandlers.size(); i++) {
			requestHandlers[i]->getContext()->libev->runLater(boost::bind(
				configureRequestHandler, requestHandlers[i], json));
		}

		writeSimpleResponse(client, 200, &headers, "{ \"status\": \"ok\" }");
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

	bool requestBodyExceedsLimit(Client *client, Request *req, unsigned int limit = 1024 * 128) {
		return (req->bodyType == Request::RBT_CONTENT_LENGTH
				&& req->aux.bodyInfo.contentLength > limit)
			|| (req->bodyType == Request::RBT_CHUNKED
				&& req->body.size() > limit);
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

	void respondWith413(Client *client, Request *req) {
		HeaderTable headers;
		headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");
		writeSimpleResponse(client, 413, &headers, "Request body too large");
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

	void respondWith500(Client *client, Request *req, const StaticString &body) {
		HeaderTable headers;
		headers.insert(req->pool, "Cache-Control", "no-cache, no-store, must-revalidate");
		headers.insert(req->pool, "Content-Type", "text/plain; charset=utf-8");
		writeSimpleResponse(client, 500, &headers, body);
		if (!req->ended()) {
			endRequest(&client, &req);
		}
	}

protected:
	virtual void onRequestBegin(Client *client, Request *req) {
		TRACE_POINT();
		StaticString path = req->getPathWithoutQueryString();

		P_INFO("Admin request: " << http_method_str(req->method) <<
			" " << StaticString(req->path.start->data, req->path.size));

		try {
			if (path == P_STATIC_STRING("/server.json")) {
				processServerStatus(client, req);
			} else if (regex_match(path, serverConnectionPath)) {
				processServerConnectionOperation(client, req);
			} else if (path == P_STATIC_STRING("/pool.xml")) {
				processPoolStatusXml(client, req);
			} else if (path == P_STATIC_STRING("/pool.txt")) {
				processPoolStatusTxt(client, req);
			} else if (path == P_STATIC_STRING("/pool/restart_app_group.json")) {
				processPoolRestartAppGroup(client, req);
			} else if (path == P_STATIC_STRING("/pool/detach_process.json")) {
				processPoolDetachProcess(client, req);
			} else if (path == P_STATIC_STRING("/backtraces.txt")) {
				processBacktraces(client, req);
			} else if (path == P_STATIC_STRING("/ping.json")) {
				processPing(client, req);
			} else if (path == P_STATIC_STRING("/shutdown.json")) {
				processShutdown(client, req);
			} else if (path == P_STATIC_STRING("/gc.json")) {
				processGc(client, req);
			} else if (path == P_STATIC_STRING("/config.json")) {
				processConfig(client, req);
			} else if (path == P_STATIC_STRING("/reinherit_logs.json")) {
				processReinheritLogs(client, req);
			} else if (path == P_STATIC_STRING("/reopen_logs.json")) {
				processReopenLogs(client, req);
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
		TRACE_POINT();
		if (buffer.size() > 0) {
			// Data
			req->body.append(buffer.start, buffer.size());
			if (requestBodyExceedsLimit(client, req)) {
				respondWith413(client, req);
			}
		} else if (errcode == 0) {
			// EOF
			Json::Reader reader;
			if (reader.parse(req->body, req->jsonBody)) {
				StaticString path = req->getPathWithoutQueryString();
				try {
					if (path == P_STATIC_STRING("/pool/restart_app_group.json")) {
						processPoolRestartAppGroupBody(client, req);
					} else if (path == P_STATIC_STRING("/pool/detach_process.json")) {
						processPoolDetachProcessBody(client, req);
					} else if (path == P_STATIC_STRING("/config.json")) {
						processConfigBody(client, req);
					} else {
						P_BUG("Unknown path for body processing: " << path);
					}
				} catch (const oxt::tracable_exception &e) {
					SKC_ERROR(client, "Exception: " << e.what() << "\n" << e.backtrace());
					if (!req->ended()) {
						endRequest(&client, &req);
					}
				}
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
		req->authorization = Authorization();
		ParentClass::deinitializeRequest(client, req);
	}

public:
	vector<RequestHandler *> requestHandlers;
	AdminAccountDatabase *adminAccountDatabase;
	ApplicationPool2::PoolPtr appPool;
	string instanceDir;
	string fdPassingPassword;
	EventFd *exitEvent;
	vector<Authorization> authorizations;

	AdminServer(ServerKit::Context *context)
		: ParentClass(context),
		  serverConnectionPath("^/server/(.+)\\.json$"),
		  adminAccountDatabase(NULL),
		  exitEvent(NULL)
		{ }

	virtual StaticString getServerName() const {
		return P_STATIC_STRING("AdminServer");
	}

	virtual unsigned int getClientName(const Client *client, char *buf, size_t size) const {
		char *pos = buf;
		const char *end = buf + size - 1;
		pos = appendData(pos, end, "Adm.", 1);
		pos += uintToString(client->number, pos, end - pos);
		*pos = '\0';
		return pos - buf;
	}

	bool authorizeByUid(uid_t uid) const {
		return appPool->authorizeByUid(uid);
	}

	bool authorizeByApiKey(const ApplicationPool2::ApiKey &apiKey) const {
		return appPool->authorizeByApiKey(apiKey);
	}
};


} // namespace ServerAgent
} // namespace Passenger

#endif /* _PASSENGER_SERVER_AGENT_ADMIN_SERVER_H_ */
