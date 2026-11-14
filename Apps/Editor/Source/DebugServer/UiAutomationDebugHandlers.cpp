#include "DebugHandlerHelpers.h"

namespace GameEngine
{
void RegisterUiAutomationDebugHandlers(EditorDebugServer& server, EditorApplication& app)
{
    struct MenuAutomationState
    {
        bool armed = false;
        bool captured = false;
        uint64_t generation = 0;
        InterceptableContextMenu::Capture menu;
        // Pooled-row identification for the click that opened the capture
        // ({index, tableId}), or null when the click was not over a pooled row.
        json rowInfo;
    };
    auto menuAutomation = std::make_shared<MenuAutomationState>();
    InterceptableContextMenu::SetInterceptor(
        [menuAutomation](InterceptableContextMenu::Capture&& capture) -> bool
    {
        if (!menuAutomation->armed)
            return false; // interactive use — open the popup as usual
        menuAutomation->armed = false;
        menuAutomation->captured = true;
        ++menuAutomation->generation;
        menuAutomation->menu = std::move(capture);
        return true;
    });

    auto menuCaptureToJson = [menuAutomation]() -> json
    {
        json items = json::array();
        for (const InterceptableContextMenu::CapturedItem& item : menuAutomation->menu.Items)
        {
            json j{{"path", item.Path}};
            if (item.IsSeparator)
            {
                j["separator"] = true;
            }
            else
            {
                if (item.IsSubMenu)
                    j["submenu"] = true;
                else
                    j["commandId"] = item.CommandId;
                j["enabled"] = item.Enabled;
                if (item.Checked)
                    j["checked"] = true;
                if (!item.Icon.empty())
                    j["icon"] = item.Icon;
                if (!item.Color.empty())
                    j["color"] = item.Color;
            }
            items.push_back(std::move(j));
        }
        json result{{"ok", true},
                    {"items", std::move(items)},
                    {"x", menuAutomation->menu.X},
                    {"y", menuAutomation->menu.Y},
                    {"generation", menuAutomation->generation}};
        if (!menuAutomation->rowInfo.is_null())
            result["row"] = menuAutomation->rowInfo;
        return result;
    };

    // Resolves a pooled list from an automation id: the element may be the
    // ListView itself, a TableView (its body is the pooled list), or a
    // container holding exactly one pooled list.
    auto resolvePooledList = [](UIManager* ui, const std::string& tableId, std::string& outError) -> ListView*
    {
        UIElement* root = ui->GetRootElement();
        UIElement* el = root ? root->FindById(tableId) : nullptr;
        if (!el)
        {
            outError = "Element not found: " + tableId;
            return nullptr;
        }
        if (auto* list = dynamic_cast<ListView*>(el))
            return list;
        if (auto* table = dynamic_cast<TableView*>(el))
            return &table->Body();
        std::vector<ListView*> found;
        std::function<void(UIElement*)> walk = [&](UIElement* e)
        {
            if (auto* list = dynamic_cast<ListView*>(e))
            {
                found.push_back(list);
                return; // a list's internals cannot host another pooled-list target
            }
            for (const auto& child : e->GetChildren())
            {
                if (child)
                    walk(child.get());
            }
        };
        walk(el);
        if (found.size() == 1)
            return found.front();
        outError = found.empty()
            ? "No pooled list (ListView/TableView) under element '" + tableId + "'"
            : "Element '" + tableId + "' contains multiple pooled lists - pass the list's own id";
        return nullptr;
    };

    // Pooled-row identification for a click point. The injected mouse move
    // resolved hover through the real hit-test, so walk up from the hovered
    // leaf to the owning pooled list and ask it which logical row contains the
    // point. Null when the click was not over a bound pooled row.
    auto resolvePooledRowAtPoint = [](UIManager* ui, float x, float y) -> json
    {
        for (UIElement* el = ui->GetHoveredElement(); el; el = el->GetParent())
        {
            auto* list = dynamic_cast<ListView*>(el);
            if (!list)
                continue;
            const int index = list->GetIndexAtPoint(x, y);
            if (index < 0)
                return json();
            json row{{"index", index}};
            if (!list->GetId().empty())
                row["tableId"] = list->GetId();
            return row;
        }
        return json();
    };


server.RegisterHandler("get_ui_tree", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        int maxDepth = ctx.params.value("maxDepth", 5);
        std::string rootId = ctx.params.value("rootId", "");
        bool includeLayout = ctx.params.value("includeLayout", true);
        bool includeHidden = ctx.params.value("includeHidden", false);
        // 0 = return every text value whole, for a caller checking a long string.
        int maxTextLength = ctx.params.value("maxTextLength", Editor::kDefaultUITreeMaxTextLength);

        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");

        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");

        UIElement* root = wndCtx->ui->GetRootElement();
        if (!root)
            return Editor::RefuseRequest("No root element");

        if (!rootId.empty())
        {
            UIElement* found = root->FindById(rootId);
            if (!found)
                return Editor::RefuseRequest("Element not found: " + rootId);
            root = found;
        }

        Editor::UITreeDumpOptions options{maxDepth, includeLayout, includeHidden, maxTextLength};
        Editor::UITreeDumpCompleteness completeness;
        json tree = Editor::BuildUITreeDump(root, options, completeness);
        // A self-hidden rootId yields no node at all. Returning that bare null would answer a
        // targeted lookup with something a caller reads as "no such element", when the element
        // was found and then filtered out by this request's own default. A featureless element
        // also yields nothing, which is not an error — hence the explicit predicate rather
        // than treating every null as hidden.
        if (tree.is_null() && Editor::IsHiddenForDump(*root))
            return Editor::RefuseRequest((rootId.empty() ? std::string("The root element") : "Element '" + rootId + "'") +
                                             " is hidden (display:none or not visible), so it was filtered out of the dump. "
                                             "Pass includeHidden:true to inspect it.");
        if (tree.is_null())
            tree = json::object();
        Editor::AnnotateDumpCompleteness(tree, completeness, options, Editor::UITreeDumpLookupAdvice::RootIdAvailable);
        return tree; });

server.RegisterHandler("get_panel_tree", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        std::string panelId = ctx.params.value("panelId", "");
        if (panelId.empty())
            return Editor::RefuseRequest("Provide a 'panelId' parameter (e.g. 'Hierarchy', 'Inspector')");

        int maxDepth = ctx.params.value("maxDepth", 5);
        bool includeLayout = ctx.params.value("includeLayout", true);
        bool includeHidden = ctx.params.value("includeHidden", false);
        // 0 = return every text value whole, for a caller checking a long string.
        int maxTextLength = ctx.params.value("maxTextLength", Editor::kDefaultUITreeMaxTextLength);

        if (!app.m_Docking)
            return Editor::RefuseRequest("Docking system not initialized");

        UIElement* panel = app.m_Docking->GetPanel(panelId);
        if (!panel)
        {
            // List available panel IDs to help discovery.
            json available = json::array();
            for (const auto& [id, _] : app.m_Docking->GetPanels())
                available.push_back(id);
            return Editor::RefuseRequest("Panel not found: " + panelId, json{{"availablePanels", available}});
        }

        Editor::UITreeDumpOptions options{maxDepth, includeLayout, includeHidden, maxTextLength};
        Editor::UITreeDumpCompleteness completeness;
        json tree = Editor::BuildUITreeDump(panel, options, completeness);
        if (tree.is_null() && Editor::IsHiddenForDump(*panel))
            return Editor::RefuseRequest("Panel '" + panelId +
                                             "' is hidden (display:none or not visible), so it was filtered out of the dump. "
                                             "Pass includeHidden:true to inspect it.");
        if (tree.is_null())
            tree = json::object();
        // DepthOnly: this handler takes no rootId, so its note must not send callers to one.
        Editor::AnnotateDumpCompleteness(tree, completeness, options, Editor::UITreeDumpLookupAdvice::DepthOnly);
        return tree; });

server.RegisterHandler("click_element", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");

        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");

        auto* ui = wndCtx->ui.get();
        float x = 0.0f;
        float y = 0.0f;

        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            json targetError;
            if (!Editor::ResolveElementPointerTarget(ui->GetRootElement(), elementId, x, y, targetError))
                return targetError;
        }
        else if (ctx.params.contains("x") && ctx.params.contains("y"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "x", "y", x, y, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or x/y coordinates");
        }

        int button = 0;
        if (ctx.params.contains("button"))
        {
            if (ctx.params["button"].is_number_integer())
            {
                button = ctx.params["button"].get<int>();
            }
            else if (ctx.params["button"].is_string())
            {
                const std::string buttonName = ctx.params["button"].get<std::string>();
                if (buttonName == "left")
                    button = 0;
                else if (buttonName == "right")
                    button = 1;
                else if (buttonName == "middle")
                    button = 2;
                else
                    return Editor::RefuseRequest("button must be left, right, middle, or an integer mouse button");
            }
            else
            {
                return Editor::RefuseRequest("button must be left, right, middle, or an integer mouse button");
            }
        }
        if (button < 0)
            return Editor::RefuseRequest("button must be non-negative");

        // Frame 1: move + press.
        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), x, y);
        Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, true);

        // Defer the release to the next frame so the UI processes the press first.
        // The poll function runs inside FlushPendingRequests, which is called once
        // per frame *before* ui->Update(). We must skip the first poll (same frame
        // as the press) so that ui->Update() sees the press edge. On the second
        // poll (next frame), we issue the release before ui->Update() processes it.
        float clickX = x;
        float clickY = y;
        int clickButton = button;
        auto framesToSkip = std::make_shared<int>(1);
        server.EnqueueDeferredResponse(ctx.id, [wndCtx, clickX, clickY, clickButton, framesToSkip](json& outResult) -> bool
        {
            if (*framesToSkip > 0)
            {
                --(*framesToSkip);
                return false; // Not ready yet — wait for next frame.
            }
            if (!wndCtx || !wndCtx->ui)
            {
                outResult = Editor::RefuseRequest("UI manager went away before the click could be released");
                return true;
            }
            // This poll runs after the press edge's ui->Update(), so hit-testing has resolved
            // who the press was dispatched to. "clicked" only ever meant "pointer input was
            // injected at these coordinates"; report the element that actually received it,
            // the way move_pointer reports its hover, so a click that landed somewhere other
            // than the requested target is visible instead of reading as a success.
            //
            // Reported as information, not as a pass/fail verdict on the click: the resolved
            // hover is not stable enough to convict on. Two identical clicks on one dropdown
            // were measured returning different hits, the second an empty string, so a
            // boolean would have asserted a confident false negative on a click that worked.
            const std::string hit = wndCtx->ui->GetHoveredElementDebugName();
            Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), clickX, clickY);
            Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), clickButton, false);
            outResult = json{{"clicked", true}, {"x", clickX}, {"y", clickY}, {"button", clickButton}, {"hit", hit}};
            return true;
        });

        return EditorDebugServer::DeferredMarker(); });

server.RegisterHandler("move_pointer", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");

        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");

        auto* ui = wndCtx->ui.get();
        float x = 0.0f;
        float y = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            json targetError;
            if (!Editor::ResolveElementPointerTarget(ui->GetRootElement(), elementId, x, y, targetError))
                return targetError;
        }
        else if (ctx.params.contains("x") && ctx.params.contains("y"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "x", "y", x, y, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or x/y coordinates");
        }

        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), x, y);

        // Wait until Update() has resolved the injected position, then report
        // the actual leaf hit target. This makes hover regressions observable
        // through MCP instead of relying only on screenshot timing.
        auto framesToSkip = std::make_shared<int>(1);
        server.EnqueueDeferredResponse(ctx.id, [ui, x, y, framesToSkip](json& outResult) -> bool
        {
            if (*framesToSkip > 0)
            {
                --(*framesToSkip);
                return false;
            }
            outResult = json{{"moved", true},
                             {"x", x},
                             {"y", y},
                             {"hovered", ui->GetHoveredElementDebugName()}};
            return true;
        });
        return EditorDebugServer::DeferredMarker(); });

server.RegisterHandler("open_context_menu", [&app, &server, menuAutomation, menuCaptureToJson, resolvePooledList, resolvePooledRowAtPoint](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");
        auto* ui = wndCtx->ui.get();

        const std::string buttonName = ctx.params.value("button", std::string("right"));
        int button = 1;
        if (buttonName == "right")
            button = 1;
        else if (buttonName == "left")
            button = 0;
        else
            return Editor::RefuseRequest("button must be right or left");

        const bool hasTableId = ctx.params.contains("tableId");
        const bool hasRowIndex = ctx.params.contains("rowIndex");
        if (hasTableId != hasRowIndex)
            return Editor::RefuseRequest("tableId and rowIndex must be provided together");

        constexpr int kMaxFramesWaitingForMenu = 25;

        if (hasTableId)
        {
            const std::string tableId = ctx.params["tableId"].get<std::string>();
            const int rowIndex = ctx.params["rowIndex"].get<int>();

            std::string resolveError;
            ListView* list = resolvePooledList(ui, tableId, resolveError);
            if (!list)
                return Editor::RefuseRequest(resolveError);
            const int count = list->GetItemCount();
            if (rowIndex < 0 || rowIndex >= count)
                return Editor::RefuseRequest("rowIndex " + std::to_string(rowIndex) + " out of range (list '"
                                                 + tableId + "' has " + std::to_string(count) + " items)");

            // The logical row may be outside the bound window; scroll it into
            // view and let the next virtualization pass bind a pooled cell.
            list->ScrollIndexIntoView(rowIndex);

            // Phase machine: bind-wait -> press -> release -> capture-wait. The
            // list is re-resolved by id every poll so a panel rebuild between
            // frames cannot leave a dangling pointer. Press/release keep the
            // click_element cadence (down edge gets a full ui->Update() before
            // the up edge).
            struct RowClickState
            {
                int phase = 0;         // polls since enqueue (first runs on the press-frame flush)
                int pressedPhase = -1; // poll at which the press was injected
                float clickX = 0.0f;
                float clickY = 0.0f;
            };
            auto st = std::make_shared<RowClickState>();
            constexpr int kMaxFramesWaitingForBind = 15;
            server.EnqueueDeferredResponse(ctx.id,
                [ui, wndCtx, menuAutomation, menuCaptureToJson, resolvePooledList, tableId, rowIndex, button, st](json& outResult) -> bool
            {
                ++st->phase;
                if (st->pressedPhase < 0)
                {
                    std::string resolveError;
                    ListView* list = resolvePooledList(ui, tableId, resolveError);
                    if (!list)
                    {
                        outResult = Editor::RefuseRequest(resolveError);
                        return true;
                    }
                    UIElement* cell = list->GetCellForIndex(rowIndex);
                    if (cell && cell->GetLayoutWidth() > 0.0f && cell->GetLayoutHeight() > 0.0f)
                    {
                        st->clickX = cell->GetLayoutX() + cell->GetLayoutWidth() * 0.5f;
                        st->clickY = cell->GetLayoutY() + cell->GetLayoutHeight() * 0.5f;
                        menuAutomation->armed = true;
                        menuAutomation->captured = false;
                        menuAutomation->menu = {};
                        menuAutomation->rowInfo = json{{"index", rowIndex}, {"tableId", tableId}};
                        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), st->clickX, st->clickY);
                        Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, true);
                        st->pressedPhase = st->phase;
                        return false;
                    }
                    if (st->phase > kMaxFramesWaitingForBind)
                    {
                        outResult = Editor::RefuseRequest("Row " + std::to_string(rowIndex) + " of '" + tableId
                                                              + "' did not bind within " + std::to_string(kMaxFramesWaitingForBind)
                                                              + " frames — is the list visible?");
                        return true;
                    }
                    return false;
                }
                if (st->phase == st->pressedPhase + 1)
                {
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), st->clickX, st->clickY);
                    Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, false);
                    return false;
                }
                if (menuAutomation->captured)
                {
                    outResult = menuCaptureToJson();
                    outResult["clickX"] = st->clickX;
                    outResult["clickY"] = st->clickY;
                    return true;
                }
                if (st->phase > st->pressedPhase + kMaxFramesWaitingForMenu)
                {
                    menuAutomation->armed = false;
                    outResult = Editor::RefuseRequest("No context menu opened within " + std::to_string(kMaxFramesWaitingForMenu)
                                                          + " frames — does row " + std::to_string(rowIndex) + " of '" + tableId
                                                          + "' have a context menu?");
                    return true;
                }
                return false;
            });
            return EditorDebugServer::DeferredMarker();
        }

        float x = 0.0f;
        float y = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            json targetError;
            if (!Editor::ResolveElementPointerTarget(ui->GetRootElement(), elementId, x, y, targetError))
                return targetError;
        }
        else if (ctx.params.contains("x") && ctx.params.contains("y"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "x", "y", x, y, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId, x/y coordinates, or tableId+rowIndex");
        }

        menuAutomation->armed = true;
        menuAutomation->captured = false;
        menuAutomation->menu = {};
        menuAutomation->rowInfo = json();

        // Same press-now / release-next-frame cadence as click_element; the
        // menu opens inside ui->Update() when the click handler runs, and the
        // armed interceptor captures it in the same frame.
        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), x, y);
        Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, true);

        auto phase = std::make_shared<int>(0);
        const float clickX = x;
        const float clickY = y;
        server.EnqueueDeferredResponse(ctx.id,
            [ui, wndCtx, menuAutomation, menuCaptureToJson, resolvePooledRowAtPoint, clickX, clickY, button, phase](json& outResult) -> bool
        {
            ++(*phase);
            // Deferred polls run in the SAME FlushPendingRequests as the
            // handler that queued them, so phase 1 is still the press frame —
            // releasing here would erase the down edge before ui->Update()
            // ever sees it (same skip click_element does). Release on the
            // next frame's poll instead.
            if (*phase == 1)
                return false;
            if (*phase == 2)
            {
                Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), clickX, clickY);
                Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, false);
                return false;
            }
            if (menuAutomation->captured)
            {
                // Identify the pooled row under the click (if any) so scripts
                // can verify which logical row's menu was captured.
                menuAutomation->rowInfo = resolvePooledRowAtPoint(ui, clickX, clickY);
                outResult = menuCaptureToJson();
                return true;
            }
            if (*phase > kMaxFramesWaitingForMenu)
            {
                menuAutomation->armed = false;
                outResult = Editor::RefuseRequest("No context menu opened within " + std::to_string(kMaxFramesWaitingForMenu)
                                                      + " frames — is the target menu-clickable?");
                return true;
            }
            return false;
        });
        return EditorDebugServer::DeferredMarker(); });

server.RegisterHandler("get_context_menu", [menuAutomation, menuCaptureToJson](const EditorDebugServer::RequestContext& /*ctx*/) -> json
                           {
        if (!menuAutomation->captured)
            return json{{"open", false}};
        json result = menuCaptureToJson();
        result["open"] = true;
        return result; });

server.RegisterHandler("invoke_menu_item", [&app, menuAutomation](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (!menuAutomation->captured)
            return Editor::RefuseRequest("No captured context menu — call open_context_menu first");

        const InterceptableContextMenu::CapturedItem* chosen = nullptr;
        if (ctx.params.contains("commandId"))
        {
            const uint32_t commandId = ctx.params["commandId"].get<uint32_t>();
            for (const auto& item : menuAutomation->menu.Items)
            {
                if (!item.IsSeparator && !item.IsSubMenu && item.CommandId == commandId)
                {
                    chosen = &item;
                    break;
                }
            }
            if (!chosen)
                return Editor::RefuseRequest("No menu item with commandId " + std::to_string(commandId));
        }
        else if (ctx.params.contains("path"))
        {
            const std::string pathParam = ctx.params["path"].get<std::string>();
            int leafMatches = 0;
            const InterceptableContextMenu::CapturedItem* leafHit = nullptr;
            for (const auto& item : menuAutomation->menu.Items)
            {
                if (item.IsSeparator || item.IsSubMenu)
                    continue;
                if (item.Path == pathParam)
                {
                    chosen = &item;
                    break;
                }
                const size_t slash = item.Path.find_last_of('/');
                const std::string leaf =
                    slash == std::string::npos ? item.Path : item.Path.substr(slash + 1);
                if (leaf == pathParam)
                {
                    ++leafMatches;
                    leafHit = &item;
                }
            }
            if (!chosen && leafMatches == 1)
                chosen = leafHit;
            if (!chosen)
                return Editor::RefuseRequest(leafMatches > 1
                           ? "Menu title '" + pathParam + "' is ambiguous — pass the full path or commandId"
                           : "No menu item matches '" + pathParam + "'");
        }
        else
        {
            return Editor::RefuseRequest("Provide commandId or path");
        }

        if (!chosen->Enabled)
            return Editor::RefuseRequest("Menu item is disabled: " + chosen->Path);
        if (!menuAutomation->menu.Invoke)
            return Editor::RefuseRequest("Captured menu has no command handler");

        const uint32_t commandId = chosen->CommandId;
        const std::string itemPath = chosen->Path;
        // Consume the capture before invoking: the callback may rebuild UI or
        // open follow-up flows, and a stale capture must not stay addressable.
        auto invoke = std::move(menuAutomation->menu.Invoke);
        menuAutomation->captured = false;
        menuAutomation->menu = {};
        menuAutomation->rowInfo = json();

        // Bind the window's UI context so callbacks that post deferred UI work
        // (dispatcher/scheduler TLS) behave exactly as they do when invoked
        // from the popup inside UI event dispatch.
        UIManager* ui = !app.m_Windows.empty() && app.m_Windows[0] ? app.m_Windows[0]->ui.get() : nullptr;
        if (ui)
        {
            UI::UiContextScope uiScope(ui->GetDispatcher(), ui->GetScheduler());
            invoke(commandId);
        }
        else
        {
            invoke(commandId);
        }
        return json{{"ok", true}, {"invoked", itemPath}, {"commandId", commandId}}; });

server.RegisterHandler("set_text_field", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");

        if (!ctx.params.contains("elementId") || !ctx.params.contains("text"))
            return Editor::RefuseRequest("Provide elementId and text");
        const std::string elementId = ctx.params["elementId"].get<std::string>();
        const std::string text = ctx.params["text"].get<std::string>();

        UIElement* root = wndCtx->ui->GetRootElement();
        UIElement* target = root ? root->FindById(elementId) : nullptr;
        if (!target)
            return Editor::RefuseRequest("Element not found: " + elementId);
        auto* field = dynamic_cast<TextField*>(target);
        if (!field)
            return Editor::RefuseRequest("Element is not a TextField: " + elementId);

        const std::string previous = field->GetValue();
        field->SetValue(text);
        return json{{"ok", true}, {"elementId", elementId}, {"previous", previous}}; });

server.RegisterHandler("input_text", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");
        auto* ui = wndCtx->ui.get();

        if (!ctx.params.contains("text") || !ctx.params["text"].is_string())
            return Editor::RefuseRequest("Provide text (string)");
        const std::string text = ctx.params["text"].get<std::string>();

        // Decode UTF-8 up-front so malformed input fails before any event is queued.
        std::vector<uint32_t> codepoints;
        codepoints.reserve(text.size());
        for (size_t i = 0; i < text.size();)
        {
            const unsigned char c0 = static_cast<unsigned char>(text[i]);
            uint32_t cp = 0;
            size_t len = 0;
            if (c0 < 0x80) { cp = c0; len = 1; }
            else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1Fu; len = 2; }
            else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0Fu; len = 3; }
            else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07u; len = 4; }
            else
                return Editor::RefuseRequest("text is not valid UTF-8");
            if (i + len > text.size())
                return Editor::RefuseRequest("text is not valid UTF-8");
            for (size_t k = 1; k < len; ++k)
            {
                const unsigned char cc = static_cast<unsigned char>(text[i + k]);
                if ((cc & 0xC0) != 0x80)
                    return Editor::RefuseRequest("text is not valid UTF-8");
                cp = (cp << 6) | (cc & 0x3Fu);
            }
            if (cp == '\n' || cp == '\r' || !Utf8::IsTextInputCodepoint(cp))
                return Editor::RefuseRequest("text contains a control character the UI would drop — use commit:true for Enter, send_key for others");
            codepoints.push_back(cp);
            i += len;
        }

        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            UIElement* root = ui->GetRootElement();
            UIElement* target = root ? root->FindById(elementId) : nullptr;
            if (!target)
                return Editor::RefuseRequest("Element not found: " + elementId);
            ui->FocusElement(target);
        }
        if (ui->GetFocusedElementId().empty())
            return Editor::RefuseRequest("No focused element — pass elementId or focus one first");
        const std::string focusId = ui->GetFocusedElementId();

        if (ctx.params.value("clear", false))
        {
            // Select-all through the same key path as Ctrl+A so the typed text
            // replaces the content; the release keeps mod-held state balanced.
            ui->OnKey('A', 1 /*press*/, 0x0002 /*ctrl*/);
            ui->OnKey('A', 0 /*release*/, 0);
        }

        for (const uint32_t cp : codepoints)
            ui->OnChar(cp);

        const bool commit = ctx.params.value("commit", false);
        constexpr int kKeyEnter = 257; // GLFW_KEY_ENTER
        if (commit)
            ui->OnKey(kKeyEnter, 1 /*press*/, 0);

        // Defer the response one frame so ui->Update() has dispatched the whole
        // queued sequence (focus-in, select-all, chars, Enter press) before the
        // field value is read back; the Enter release mirrors send_key's tap.
        auto framesToSkip = std::make_shared<int>(1);
        const size_t cpCount = codepoints.size();
        server.EnqueueDeferredResponse(ctx.id, [ui, focusId, commit, cpCount, framesToSkip](json& outResult) -> bool
        {
            if (*framesToSkip > 0)
            {
                --(*framesToSkip);
                return false;
            }
            if (commit)
                ui->OnKey(kKeyEnter, 0 /*release*/, 0);

            outResult = json{{"ok", true},
                             {"focusId", focusId},
                             {"codepoints", cpCount},
                             {"committed", commit}};
            // Best-effort readback of the focused field's value so callers can
            // verify what the control now holds without a second round-trip.
            UIElement* root = ui->GetRootElement();
            UIElement* el = root ? root->FindById(focusId) : nullptr;
            if (auto* sf = dynamic_cast<Field<std::string>*>(el))
                outResult["value"] = sf->GetValue();
            else if (auto* ff = dynamic_cast<Field<float>*>(el))
                outResult["value"] = ff->GetValue();
            else if (auto* iff = dynamic_cast<Field<int>*>(el))
                outResult["value"] = iff->GetValue();
            return true;
        });
        return EditorDebugServer::DeferredMarker(); });

server.RegisterHandler("send_key", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");
        auto* ui = wndCtx->ui.get();

        if (!ctx.params.contains("key"))
            return Editor::RefuseRequest("Provide key (name or GLFW keycode integer)");

        auto toLower = [](std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; };

        int keyCode = -1;
        const auto& kparam = ctx.params["key"];
        if (kparam.is_number_integer())
        {
            keyCode = kparam.get<int>();
        }
        else if (kparam.is_string())
        {
            const std::string name = kparam.get<std::string>();
            static const std::unordered_map<std::string, int> kNamed = {
                {"tab", 258}, {"enter", 257}, {"return", 257}, {"numpadenter", 335},
                {"escape", 256}, {"esc", 256}, {"space", 32}, {"backspace", 259},
                {"delete", 261}, {"home", 268}, {"end", 269},
                {"up", 265}, {"down", 264}, {"left", 263}, {"right", 262}};
            auto it = kNamed.find(toLower(name));
            if (it != kNamed.end())
                keyCode = it->second;
            else if (name.size() == 1)
            {
                const char c = name[0];
                if (c >= 'a' && c <= 'z') keyCode = 'A' + (c - 'a');
                else if (c >= 'A' && c <= 'Z') keyCode = c;
                else if (c >= '0' && c <= '9') keyCode = c;
            }
            if (keyCode < 0)
                return Editor::RefuseRequest("Unknown key name: " + name);
        }
        else
        {
            return Editor::RefuseRequest("key must be a string name or integer keycode");
        }

        int mods = 0;
        auto applyMod = [&mods, &toLower](const std::string& m) {
            const std::string s = toLower(m);
            if (s == "shift") mods |= 0x0001;
            else if (s == "control" || s == "ctrl") mods |= 0x0002;
            else if (s == "alt") mods |= 0x0004;
            else if (s == "super" || s == "cmd" || s == "meta") mods |= 0x0008;
        };
        if (ctx.params.contains("mods"))
        {
            const auto& mp = ctx.params["mods"];
            if (mp.is_string())
                applyMod(mp.get<std::string>());
            else if (mp.is_array())
                for (const auto& e : mp) { if (e.is_string()) applyMod(e.get<std::string>()); }
            else if (mp.is_number_integer())
                mods = mp.get<int>();
        }

        const std::string action = ctx.params.value("action", std::string("tap"));
        constexpr int kRelease = 0, kPress = 1, kRepeat = 2;

        if (action == "press")
        {
            Editor::InjectKey(wndCtx->inputConfig, ui, keyCode, kPress, mods);
            return json{{"ok", true}, {"key", keyCode}, {"action", "press"}, {"mods", mods}};
        }
        if (action == "release")
        {
            Editor::InjectKey(wndCtx->inputConfig, ui, keyCode, kRelease, mods);
            return json{{"ok", true}, {"key", keyCode}, {"action", "release"}, {"mods", mods}};
        }
        if (action == "repeat")
        {
            Editor::InjectKey(wndCtx->inputConfig, ui, keyCode, kRepeat, mods);
            return json{{"ok", true}, {"key", keyCode}, {"action", "repeat"}, {"mods", mods}};
        }
        if (action != "tap")
            return Editor::RefuseRequest("action must be tap, press, release, or repeat");

        int count = ctx.params.value("count", 1);
        if (count < 1) count = 1;
        if (count > 64) count = 64;

        // Press (count times) this frame; defer the release to the next frame so
        // ui->Update() processes the press edge(s) first (mirrors click_element).
        for (int i = 0; i < count; ++i)
            Editor::InjectKey(wndCtx->inputConfig, ui, keyCode, kPress, mods);

        auto framesToSkip = std::make_shared<int>(1);
        const int relKey = keyCode;
        const int relMods = mods;
        const int relCount = count;
        server.EnqueueDeferredResponse(ctx.id, [wndCtx, ui, relKey, relMods, relCount, framesToSkip](json& outResult) -> bool
        {
            if (*framesToSkip > 0)
            {
                --(*framesToSkip);
                return false;
            }
            Editor::InjectKey(wndCtx->inputConfig, ui, relKey, 0 /*release*/, relMods);
            outResult = json{{"ok", true}, {"key", relKey}, {"action", "tap"}, {"count", relCount}, {"mods", relMods}};
            return true;
        });
        return EditorDebugServer::DeferredMarker(); });

server.RegisterHandler("send_text", [&app](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");
        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");
        if (!ctx.params.contains("text") || !ctx.params["text"].is_string())
            return Editor::RefuseRequest("Provide UTF-8 string parameter 'text'");

        const std::string text = ctx.params["text"].get<std::string>();
        std::size_t at = 0;
        int sent = 0;
        while (at < text.size())
        {
            const unsigned char lead = static_cast<unsigned char>(text[at++]);
            std::uint32_t codepoint = 0;
            int continuationCount = 0;
            if (lead < 0x80) codepoint = lead;
            else if ((lead & 0xE0) == 0xC0) { codepoint = lead & 0x1F; continuationCount = 1; }
            else if ((lead & 0xF0) == 0xE0) { codepoint = lead & 0x0F; continuationCount = 2; }
            else if ((lead & 0xF8) == 0xF0) { codepoint = lead & 0x07; continuationCount = 3; }
            else return Editor::RefuseRequest("Invalid UTF-8 lead byte");

            for (int i = 0; i < continuationCount; ++i)
            {
                if (at >= text.size())
                    return Editor::RefuseRequest("Truncated UTF-8 sequence");
                const unsigned char next = static_cast<unsigned char>(text[at++]);
                if ((next & 0xC0) != 0x80)
                    return Editor::RefuseRequest("Invalid UTF-8 continuation byte");
                codepoint = (codepoint << 6) | (next & 0x3F);
            }
            if (Editor::InjectChar(wndCtx->inputConfig, wndCtx->ui.get(), codepoint))
                ++sent;
        }
        return json{{"ok", true}, {"codepoints", sent},
                    {"focusedElementId", wndCtx->ui->GetFocusedElementId()}}; });

server.RegisterHandler("show_universal_search", [&app](const EditorDebugServer::RequestContext&) -> json
                           {
        if (app.m_UniversalSearch)
            app.m_UniversalSearch->Toggle();
        return json{{"ok", true},
                    {"open", app.m_UniversalSearch && app.m_UniversalSearch->IsOpen()}}; });

server.RegisterHandler("simulate_mouse_drag", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        const int windowIndex = ctx.params.value("windowIndex", 0);
        if (windowIndex < 0 || windowIndex >= static_cast<int>(app.m_Windows.size()))
            return Editor::RefuseRequest("windowIndex out of range");

        auto* wndCtx = app.m_Windows[static_cast<size_t>(windowIndex)].get();
        if (!wndCtx || !wndCtx->ui)
            return Editor::RefuseRequest("No UI manager available");

        auto* ui = wndCtx->ui.get();
        UIElement* root = ui->GetRootElement();
        if (!root)
            return Editor::RefuseRequest("No root element");

        float startX = 0.0f;
        float startY = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            json targetError;
            if (!Editor::ResolveElementPointerTarget(root, elementId, startX, startY, targetError))
                return targetError;
        }
        else if (ctx.params.contains("startX") && ctx.params.contains("startY"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "startX", "startY", startX, startY, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or startX/startY");
        }

        if (!ctx.params.contains("endX") || !ctx.params.contains("endY"))
            return Editor::RefuseRequest("Provide endX/endY coordinates");

        float endX = 0.0f;
        float endY = 0.0f;
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "endX", "endY", endX, endY, coordError))
                return coordError;
        }
        // Allow a longer single gesture for sustained-drag performance tests.
        // Its deferred-response budget is scoped to this request; all other
        // automation operations retain the normal 30-frame timeout.
        const int holdFrames = std::max(1, std::min(4, ctx.params.value("holdFrames", 1)));
        const int holdBeforeReleaseFrames =
            std::max(0, std::min(60, ctx.params.value("holdBeforeReleaseFrames", 0)));
        const int maxSteps = 240;
        const int steps = std::max(2, std::min(maxSteps, ctx.params.value("steps", 20)));
        const int pollBudget = holdFrames + steps + holdBeforeReleaseFrames + 4;

        int button = 0;
        if (ctx.params.contains("button"))
        {
            if (ctx.params["button"].is_number_integer())
            {
                button = ctx.params["button"].get<int>();
            }
            else if (ctx.params["button"].is_string())
            {
                const std::string buttonName = ctx.params["button"].get<std::string>();
                if (buttonName == "left")
                    button = 0;
                else if (buttonName == "right")
                    button = 1;
                else if (buttonName == "middle")
                    button = 2;
                else
                    return Editor::RefuseRequest("button must be left, right, middle, or an integer mouse button");
            }
            else
            {
                return Editor::RefuseRequest("button must be left, right, middle, or an integer mouse button");
            }
        }
        if (button < 0)
            return Editor::RefuseRequest("button must be non-negative");

        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), startX, startY);
        Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, true);

        auto frame = std::make_shared<int>(-holdFrames);
        auto pressedHovered = std::make_shared<std::string>();
        auto pressedCaptureId = std::make_shared<std::string>();
        auto pressedCaptured = std::make_shared<bool>(false);
        auto dragDropSession = std::make_shared<bool>(false);
        server.EnqueueDeferredResponse(ctx.id,
            [ui, wndCtx, startX, startY, endX, endY, steps, holdBeforeReleaseFrames, button, frame,
             pressedHovered, pressedCaptureId, pressedCaptured, dragDropSession](json& outResult) -> bool
            {
                if (const auto* dragDrop = ui->GetDragDropManager(); dragDrop && dragDrop->IsDragging())
                    *dragDropSession = true;
                if (*frame < 0)
                {
                    ++(*frame);
                    return false;
                }

                if (*frame < steps)
                {
                    if (*frame == 0)
                    {
                        *pressedHovered = ui->GetHoveredElementDebugName();
                        *pressedCaptureId = ui->GetCaptureId();
                        *pressedCaptured = ui->IsMouseCaptured();
                    }
                    const float t = static_cast<float>(*frame + 1) / static_cast<float>(steps);
                    const float x = startX + (endX - startX) * t;
                    const float y = startY + (endY - startY) * t;
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), x, y);
                    ++(*frame);
                    return false;
                }

                if (*frame < steps + holdBeforeReleaseFrames)
                {
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), endX, endY);
                    ++(*frame);
                    return false;
                }

                Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), endX, endY);
                Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), button, false);
                // "dragged" reports the pointer gesture; "dragDropSession" whether it started a
                // drag-and-drop session, which is what a drop needs.
                outResult = json{{"dragged", true},
                                 {"dragDropSession", *dragDropSession},
                                 {"startX", startX},
                                 {"startY", startY},
                                 {"endX", endX},
                                 {"endY", endY},
                                 {"steps", steps},
                                 {"button", button},
                                 {"pressedHoveredElement", *pressedHovered},
                                 {"pressedMouseCaptured", *pressedCaptured},
                                 {"pressedCaptureElementId", *pressedCaptureId}};
                return true;
            }, pollBudget);

        return EditorDebugServer::DeferredMarker(); });

server.RegisterHandler("simulate_double_click_drag", [&app, &server](const EditorDebugServer::RequestContext& ctx) -> json
                           {
        if (app.m_Windows.empty() || !app.m_Windows[0]->ui)
            return Editor::RefuseRequest("No UI manager available");

        // Unlike its siblings this method takes no windowIndex and has always
        // driven the main window.
        auto* wndCtx = app.m_Windows[0].get();
        auto* ui = wndCtx->ui.get();
        UIElement* root = ui->GetRootElement();
        if (!root)
            return Editor::RefuseRequest("No root element");

        float startX = 0.0f;
        float startY = 0.0f;
        if (ctx.params.contains("elementId"))
        {
            const std::string elementId = ctx.params["elementId"].get<std::string>();
            json targetError;
            if (!Editor::ResolveElementPointerTarget(root, elementId, startX, startY, targetError))
                return targetError;
        }
        else if (ctx.params.contains("startX") && ctx.params.contains("startY"))
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "startX", "startY", startX, startY, coordError))
                return coordError;
        }
        else
        {
            return Editor::RefuseRequest("Provide elementId or startX/startY");
        }

        if (!ctx.params.contains("endX") || !ctx.params.contains("endY"))
            return Editor::RefuseRequest("Provide endX/endY coordinates");

        float endX = 0.0f;
        float endY = 0.0f;
        {
            json coordError;
            if (!Editor::ReadPointerCoords(ctx.params, "endX", "endY", endX, endY, coordError))
                return coordError;
        }
        const int gapFrames = std::max(1, std::min(8, ctx.params.value("gapFrames", 2)));
        const int holdFrames = std::max(0, std::min(4, ctx.params.value("holdFrames", 1)));
        const int maxSteps = std::max(2, 24 - gapFrames - holdFrames);
        const int steps = std::max(2, std::min(maxSteps, ctx.params.value("steps", 16)));

        Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), startX, startY);
        Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), 0, true);

        auto frame = std::make_shared<int>(0);
        server.EnqueueDeferredResponse(ctx.id,
            [wndCtx, startX, startY, endX, endY, gapFrames, holdFrames, steps, frame](json& outResult) -> bool
            {
                if (*frame == 0)
                {
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), startX, startY);
                    Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), 0, false);
                    ++(*frame);
                    return false;
                }

                if (*frame <= gapFrames)
                {
                    ++(*frame);
                    return false;
                }

                if (*frame == gapFrames + 1)
                {
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), startX, startY);
                    Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), 0, true);
                    ++(*frame);
                    return false;
                }

                if (*frame <= gapFrames + 1 + holdFrames)
                {
                    ++(*frame);
                    return false;
                }

                const int dragFrame = *frame - (gapFrames + 2 + holdFrames);
                if (dragFrame < steps)
                {
                    const float t = static_cast<float>(dragFrame + 1) / static_cast<float>(steps);
                    const float x = startX + (endX - startX) * t;
                    const float y = startY + (endY - startY) * t;
                    Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), x, y);
                    ++(*frame);
                    return false;
                }

                Editor::InjectMouseMove(wndCtx->inputConfig, wndCtx->window.get(), wndCtx->ui.get(), endX, endY);
                Editor::InjectMouseButton(wndCtx->inputConfig, wndCtx->ui.get(), 0, false);
                outResult = json{{"doubleClickDragged", true},
                                 {"startX", startX},
                                 {"startY", startY},
                                 {"endX", endX},
                                 {"endY", endY},
                                 {"steps", steps}};
                return true;
            });

        return EditorDebugServer::DeferredMarker(); });
}
}
