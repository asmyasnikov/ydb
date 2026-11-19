#include "measurement.h"
#include "syntax/antlr_ast_codec.h"
#include "syntax/cst_codec.h"
#include "syntax/script_codec.h"
#include <ydb/tests/tools/parsed_query_bench/proto/execute_query.pb.h>

#include <google/protobuf/util/message_differencer.h>

#include <util/generic/yexception.h>
#include <util/stream/file.h>
#include <util/stream/output.h>
#include <util/string/cast.h>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

using TRequest = Ydb::Query::Benchmark::ExecuteQueryRequest;

void CheckTextWireSchema() {
    const auto* published = Ydb::Query::ExecuteQueryRequest::descriptor();
    const auto* experimental = TRequest::descriptor();
    Y_ENSURE(experimental->field_count() == published->field_count() + 1,
             "Update the benchmark request to match the published schema");
    for (int i = 0; i < published->field_count(); ++i) {
        const auto* field = published->field(i);
        const auto* other = experimental->FindFieldByNumber(field->number());
        Y_ENSURE(other && field->name() == other->name() && field->type() == other->type()
            && field->is_repeated() == other->is_repeated() && field->is_map() == other->is_map()
            && field->enum_type() == other->enum_type()
            && (field->containing_oneof() != nullptr) == (other->containing_oneof() != nullptr),
            "Benchmark wire schema differs for " << field->name());
        if (field->is_map()) {
            const auto* map = field->message_type();
            const auto* otherMap = other->message_type();
            Y_ENSURE(map->map_key()->type() == otherMap->map_key()->type()
                && map->map_value()->message_type() == otherMap->map_value()->message_type(),
                "Benchmark parameter map differs from the published schema");
        } else {
            Y_ENSURE(field->message_type() == other->message_type(), "Benchmark field type differs for " << field->name());
        }
    }
}

TString ReadInput(const char* path) {
    TFile file(path, OpenExisting | RdOnly);
    Y_ENSURE(file.GetLength() >= 0 && ui64(file.GetLength()) <= MaxRequestBytes, "Input exceeds 16 MiB");
    return TFileInput(file).ReadAll();
}

TStringBuf Stage(const TRequest& request) {
    if (request.has_query_content()) {
        Y_ENSURE(request.query_content().syntax() == Ydb::Query::SYNTAX_YQL_V1, "Expected YQL v1");
        return "text";
    }
    switch (request.parsed_query().stage_case()) {
        case Ydb::Query::ParsedQuery::kAntlrAst: return "antlr_ast";
        case Ydb::Query::ParsedQuery::kAntlrCst: return "antlr_cst";
        case Ydb::Query::ParsedQuery::kScript: return "script";
        case Ydb::Query::ParsedQuery::kNativeQuery: return "native_query";
        default: ythrow yexception() << "Missing or unrecognized ParsedQuery stage";
    }
}

TString Prepare(TStringBuf stage, const TString& sql, const TRequest& envelope) {
    TRequest request(envelope);
    request.clear_query();
    if (stage == "text") {
        auto* content = request.mutable_query_content();
        content->set_syntax(Ydb::Query::SYNTAX_YQL_V1);
        content->set_text(sql);
    } else if (stage == "antlr_ast") {
        *request.mutable_parsed_query()->mutable_antlr_ast() = ExportAntlrAst(sql, {});
    } else if (stage == "antlr_cst") {
        *request.mutable_parsed_query()->mutable_antlr_cst() = ExportAntlrCst(sql, {});
    } else if (stage == "script") {
        *request.mutable_parsed_query()->mutable_script() = ExportSyntaxScript(sql, {});
    } else {
        ythrow yexception() << "Unsupported syntax stage: " << stage;
    }
    TString bytes;
    Y_ENSURE(request.SerializeToString(&bytes) && bytes.size() <= MaxRequestBytes, "Cannot encode request within 16 MiB");
    return bytes;
}

std::unique_ptr<NNewYql::TScript> ToScript(const TRequest& request) {
    if (request.has_query_content()) {
        Stage(request);
        return ParseText(request.query_content().text(), {});
    }
    const auto& input = request.parsed_query();
    switch (input.stage_case()) {
        case Ydb::Query::ParsedQuery::kAntlrAst: return ImportAntlrAst(input.antlr_ast());
        case Ydb::Query::ParsedQuery::kAntlrCst: return ImportAntlrCst(input.antlr_cst());
        case Ydb::Query::ParsedQuery::kScript: return ImportScript(input.script());
        default: ythrow yexception() << "Only text, antlr_ast, antlr_cst and script produce a syntax tree";
    }
}

struct TSample {
    size_t Bytes;
    TMeasurementTime Start, Decoded, Done;
};

struct TInput {
    TString Label;
    TString Bytes;
};

int Run(int argc, char** argv) {
    const TStringBuf mode = argc > 1 ? argv[1] : "";
    if (mode == "prepare" && (argc == 5 || argc == 6)) {
        TRequest envelope;
        if (argc == 6) {
            DecodeRequest(ReadInput(argv[5]), envelope);
        } else {
            envelope.set_exec_mode(Ydb::Query::EXEC_MODE_EXECUTE);
        }
        const auto bytes = Prepare(argv[2], ReadInput(argv[3]), envelope);
        TFileOutput(argv[4]).Write(bytes);
        return 0;
    }
    if (mode == "verify" && argc >= 4) {
        const auto expected = ExportScript(*ParseText(ReadInput(argv[2]), {}));
        TRequest envelope;
        for (int i = 3; i < argc; ++i) {
            TRequest request;
            DecodeRequest(ReadInput(argv[i]), request);
            const auto restored = ExportScript(*ToScript(request));
            Y_ENSURE(google::protobuf::util::MessageDifferencer::Equals(expected, restored),
                     "Imported syntax tree differs from the text path for " << Stage(request));
            request.clear_query();
            if (i == 3) {
                envelope = std::move(request);
            } else {
                Y_ENSURE(google::protobuf::util::MessageDifferencer::Equals(envelope, request),
                         "Request context or parameters differ between stages");
            }
        }
        return 0;
    }
    if (argc < 5 || (mode != "syntax" && mode != "decode" && mode != "client")) {
        Cerr << "prepare STAGE SQL OUTPUT.pb [REQUEST_TEMPLATE.pb]\n"
             << "verify SQL REQUEST.pb...\n"
             << "syntax WARMUP REPEATS REQUEST.pb...\n"
             << "decode WARMUP REPEATS TEXT_REQUEST.pb\n"
             << "client WARMUP REPEATS SQL [--template REQUEST.pb] STAGE...\n";
        return 2;
    }
    if (mode == "decode") {
        CheckTextWireSchema();
    }
    const ui32 warmup = FromString<ui32>(argv[2]);
    const ui32 repeats = FromString<ui32>(argv[3]);
    Y_ENSURE(repeats && repeats <= 1'000'000 && warmup <= 1'000'000, "Invalid iteration count");
    TVector<TInput> cases;
    TString sql;
    TRequest envelope;
    envelope.set_exec_mode(Ydb::Query::EXEC_MODE_EXECUTE);
    if (mode == "client") {
        Y_ENSURE(argc >= 6, "Provide a SQL file and at least one stage");
        sql = ReadInput(argv[4]);
        int firstStage = 5;
        if (TStringBuf(argv[5]) == "--template") {
            Y_ENSURE(argc >= 8, "Provide a template and at least one stage");
            DecodeRequest(ReadInput(argv[6]), envelope);
            envelope.clear_query();
            firstStage = 7;
        }
        for (int i = firstStage; i < argc; ++i) {
            cases.push_back({argv[i], {}});
        }
    } else {
        for (int i = 4; i < argc; ++i) {
            auto bytes = ReadInput(argv[i]);
            TRequest request;
            DecodeRequest(bytes, request);
            TString label(Stage(request));
            if (mode == "decode") {
                Y_ENSURE(argc == 5 && label == "text", "Decode A/B requires one text request");
                cases.push_back({"old", bytes});
                label = "new";
            }
            cases.push_back({label, std::move(bytes)});
        }
    }
    Cout << "mode,stage,iteration,bytes,wall_ns,cpu_ns,decode_wall_ns,decode_cpu_ns\n";
    for (ui32 round = 0; round < warmup + repeats; ++round) {
        for (size_t j = 0; j < cases.size(); ++j) {
            const auto& item = cases[(j + round) % cases.size()];
            TSample sample;
            sample.Bytes = item.Bytes.size();
            if (mode == "client") {
                sample.Start = sample.Decoded = TMeasurementTime::Now();
                const auto bytes = Prepare(item.Label, sql, envelope);
                sample.Done = TMeasurementTime::Now();
                sample.Bytes = bytes.size();
            } else if (mode == "decode" && item.Label == "old") {
                sample.Start = TMeasurementTime::Now();
                Ydb::Query::ExecuteQueryRequest original;
                DecodeRequest(item.Bytes, original);
                sample.Done = sample.Decoded = TMeasurementTime::Now();
            } else {
                sample.Start = TMeasurementTime::Now();
                TRequest request;
                DecodeRequest(item.Bytes, request);
                sample.Done = sample.Decoded = TMeasurementTime::Now();
                if (mode == "syntax") {
                    const auto ast = ToScript(request);
                    sample.Done = TMeasurementTime::Now();
                    Y_ENSURE(ast && !ast->Statements.empty(), "No statements");
                }
            }
            if (round >= warmup) {
                Cout << mode << ',' << item.Label << ',' << round - warmup << ',' << sample.Bytes
                     << ',' << sample.Done.Wall - sample.Start.Wall << ',' << sample.Done.Cpu - sample.Start.Cpu
                     << ',' << sample.Decoded.Wall - sample.Start.Wall << ',' << sample.Decoded.Cpu - sample.Start.Cpu << '\n';
            }
        }
    }
    return 0;
}

} // namespace
} // namespace NKikimr::NKqp::NParsedQueryBench

int main(int argc, char** argv) {
    try {
        return NKikimr::NKqp::NParsedQueryBench::Run(argc, argv);
    } catch (const std::exception& e) {
        Cerr << e.what() << Endl;
        return 1;
    }
}
