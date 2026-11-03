#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterMovieRecordDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    auto debugMoviePath = std::make_shared<std::filesystem::path>();
server.RegisterHandler("movie_record", [&app, debugMoviePath](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0] || !app.m_Windows[0]->gameView)
            return Editor::RefuseRequest("No game view available");
        auto* gameView = app.m_Windows[0]->gameView.get();

        const std::string action = ctx.params.value("action", "status");
        if (action == "start")
        {
            const std::string path = ctx.params.value("path", std::string{});
            if (path.empty())
                return Editor::RefuseRequest("Provide 'path'");
            Video::VideoWriterOptions options{};
            options.path = path;
            options.width = ctx.params.value("width", 1280u);
            options.height = ctx.params.value("height", 720u);
            options.fps = ctx.params.value("fps", 60.0);
            options.codec = Video::GuessCodecForMoviePath(options.path);
            options.requireHardwareAcceleration = false;
            const std::string hdr = ctx.params.value("hdr", std::string("off"));
            if (hdr == "pq" || hdr == "hdr10" || hdr == "pq10")
            {
                options.hdrMode = Video::VideoHdrMode::HDR10_PQ;
                options.codec = Video::VideoCodec::HEVC;
            }
            else if (hdr == "hlg")
            {
                options.hdrMode = Video::VideoHdrMode::HLG;
                options.codec = Video::VideoCodec::HEVC;
            }
            const uint64_t frameLimit = ctx.params.value("frames", 0ull);
            std::string error;
            if (!gameView->GetMovieCapture().StartRecording(options, frameLimit, &error))
                return Editor::RefuseRequest(error.empty() ? "StartRecording failed" : error);
            *debugMoviePath = path;
            return json{{"ok", true}, {"recording", true}};
        }
        if (action == "stop")
        {
            gameView->GetMovieCapture().StopRecording("Recording stopped via debug server.",
                                         /*waitForFinalization=*/true);
            const auto completedPath = *debugMoviePath;
            debugMoviePath->clear();
            if (!completedPath.empty() && app.m_MovieRecorder)
                app.m_MovieRecorder->NotifyExternalRecordingFinished(completedPath);
            return json{{"ok", true}, {"recording", gameView->GetMovieCapture().IsRecording()},
                        {"framesWritten", gameView->GetMovieCapture().GetFramesWritten()}};
        }
        return json{{"recording", gameView->GetMovieCapture().IsRecording()},
                    {"framesWritten", gameView->GetMovieCapture().GetFramesWritten()},
                    {"width", gameView->GetMovieCapture().GetWidth()},
                    {"height", gameView->GetMovieCapture().GetHeight()}}; });
}
}
