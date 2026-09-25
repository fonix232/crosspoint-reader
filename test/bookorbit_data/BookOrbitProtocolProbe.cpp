#include <array>
#include <iostream>
#include <string>

#include "BookOrbitDataProtocol.h"
#include "BookOrbitProgressPolicy.h"

using namespace bookorbit;
int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    JsonDocument input, output;
    if (deserializeJson(input, line)) return 2;
    const std::string op = input["op"] | "";
    if (op == "parse") {
      std::array<char, 8192> buffer{};
      AnnotationReplyParser parser(
          buffer.data(), buffer.size(),
          [](void* ctx, const Annotation& a) {
            auto& doc = *static_cast<JsonDocument*>(ctx);
            JsonObject entry = doc["entries"].as<JsonArray>().add<JsonObject>();
            entry["serverId"] = a.serverId;
            entry["version"] = a.version;
            entry["datetime"] = a.datetime;
            entry["pos"] = a.pos;
            entry["text"] = a.text;
            entry["key"] = a.key;
            entry["deleted"] = a.deleted;
            return true;
          },
          &output);
      output["entries"].to<JsonArray>();
      const std::string body = input["body"] | "";
      for (size_t i = 0; i < body.size(); i += 7) parser.feed(body.data() + i, std::min<size_t>(7, body.size() - i));
      output["complete"] = parser.complete();
      output["unmatched"] = parser.unmatched();
      output["more"] = parser.more();
    } else if (op == "stats-accepted") {
      output["accepted"] = statsAccepted(input["body"].as<std::string>(), input["hash"].as<std::string>(),
                                         input["count"].as<unsigned>());
    } else if (op == "decision") {
      LocalProgressState local{true, 0, input["local"].as<uint32_t>()};
      RemoteProgressState remote{true, false, input["remoteTimestamp"].as<uint32_t>(), input["remote"].as<uint32_t>()};
      AckState ack{true,
                   0,
                   input["ackLocal"].as<uint32_t>(),
                   true,
                   input["ackTimestamp"].as<uint32_t>(),
                   input["ackRemote"].as<uint32_t>()};
      output["decision"] = static_cast<int>(decideSync(local, remote, ack));
    } else {
      deviceEnvelope(output, input["deviceId"] | "crosspoint-test", "Host integration");
      auto book = output["books"].to<JsonArray>().add<JsonObject>();
      book["hash"] = input["hash"];
      if (op == "exchange") {
        book["keysComplete"] = input["complete"] | false;
        book["keys"].set(input["keys"]);
        auto changes = book["changes"].to<JsonArray>();
        for (JsonObjectConst value : input["changes"].as<JsonArrayConst>()) {
          Annotation a;
          a.datetime = value["datetime"] | "";
          a.pos = value["pos"] | "";
          a.updated = value["updated"] | "";
          a.end = value["end"] | "";
          a.text = value["text"] | "";
          annotationChange(changes.add<JsonObject>(),
                           (input["bookmark"] | false) ? AnnotationKind::Bookmark : AnnotationKind::Highlight, a);
        }
      } else if (op == "ack") {
        book["applied"].to<JsonArray>();
        book["deleted"].to<JsonArray>();
        for (JsonObjectConst value : input["entries"].as<JsonArrayConst>()) {
          Annotation a;
          a.serverId = value["serverId"] | 0u;
          a.version = value["version"] | 0u;
          a.deleted = value["deleted"] | false;
          a.key = value["key"] | "";
          a.datetime = value["datetime"] | "";
          a.pos = value["pos"] | "";
          annotationAck(book, (input["bookmark"] | false) ? AnnotationKind::Bookmark : AnnotationKind::Highlight, a);
        }
      } else if (op == "stats") {
        book["events"].set(input["events"]);
      } else
        return 3;
    }
    serializeJson(output, std::cout);
    std::cout << '\n' << std::flush;
  }
}
