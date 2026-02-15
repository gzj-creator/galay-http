module;

#include "galay-http/module/ModulePrelude.hpp"

export module galay.http;

export {
#include "galay-http/protoc/http/HttpBase.h"
#include "galay-http/protoc/http/HttpBody.h"
#include "galay-http/protoc/http/HttpChunk.h"
#include "galay-http/protoc/http/HttpError.h"
#include "galay-http/protoc/http/HttpHeader.h"
#include "galay-http/protoc/http/HttpRequest.h"
#include "galay-http/protoc/http/HttpResponse.h"

#include "galay-http/kernel/http/HttpClient.h"
#include "galay-http/kernel/http/HttpConn.h"
#include "galay-http/kernel/http/HttpReader.h"
#include "galay-http/kernel/http/HttpRouter.h"
#include "galay-http/kernel/http/HttpServer.h"
#include "galay-http/kernel/http/HttpSession.h"
#include "galay-http/kernel/http/HttpWriter.h"

#include "galay-http/utils/Http1_1RequestBuilder.h"
#include "galay-http/utils/Http1_1ResponseBuilder.h"
#include "galay-http/utils/HttpUtils.h"
}
