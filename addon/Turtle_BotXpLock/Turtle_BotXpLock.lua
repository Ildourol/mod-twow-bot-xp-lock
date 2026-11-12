-- -----------------------------------------------------------------------------
-- Turtle_BotXpLock: Client AddOn for Turtle WoW (1.18.1 / 1.12.1)
-- Strict Lua 5.0 Runtime Compliance: No '#', No '%', No string.match
-- -----------------------------------------------------------------------------

local ADDON_PREFIX = "TW_BOT_XPLOCK"
local currentTargetGuid = 0
local currentTargetStatus = 0 -- 0: unknown/unlocked, 1: locked, 2: capped
local partyBotStatus = {} -- [partyIndex] = { isLocked = 0, lockLevel = 0, xpRate = 1.0 }
local registryBots = {}
local registryPage = 1
local ROWS_PER_PAGE = 10

-- -----------------------------------------------------------------------------
-- 1. Helper Utilities (Strict Lua 5.0)
-- -----------------------------------------------------------------------------
local function SplitString(str, delim)
    local result = {}
    local startIdx = 1
    local delimLen = string.len(delim)
    while true do
        local pos = string.find(str, delim, startIdx, true)
        if not pos then
            table.insert(result, string.sub(str, startIdx))
            break
        end
        table.insert(result, string.sub(str, startIdx, pos - 1))
        startIdx = pos + delimLen
    end
    return result
end

-- -----------------------------------------------------------------------------
-- 2. Silent Slash Chat Command Filter
-- Suppresses .botxplock commands and server error echo from chat boxes
-- -----------------------------------------------------------------------------
local origChatFrame_OnEvent = ChatFrame_OnEvent
ChatFrame_OnEvent = function(event)
    if (event == "CHAT_MSG_SAY" or event == "CHAT_MSG_WHISPER_INFORM" or event == "CHAT_MSG_WHISPER") and type(arg1) == "string" and string.find(arg1, "^%.botxplock%s*") then
        return
    end
    origChatFrame_OnEvent(event)
end

for i = 1, 7 do
    local cf = getglobal("ChatFrame" .. i)
    if cf and cf.AddMessage then
        local origAdd = cf.AddMessage
        cf.AddMessage = function(this, msg, r, g, b, id)
            if type(msg) == "string" and string.find(msg, "^%.botxplock%s*") then
                return
            end
            origAdd(this, msg, r, g, b, id)
        end
    end
end

-- -----------------------------------------------------------------------------
-- 3. Send Wire / Command Messages
-- -----------------------------------------------------------------------------
local function SendModuleMessage(action, guid, level, rate, runChatCmd)
    local cmd = ""
    if action == "LOCK" then
        if level and level > 0 then
            cmd = "lock " .. tostring(level)
        else
            cmd = "lock"
        end
    elseif action == "UNLOCK" then
        cmd = "unlock"
    elseif action == "TOGGLE" then
        cmd = "toggle"
    elseif action == "RATE" then
        cmd = "rate " .. tostring(rate or 1.0)
    elseif action == "LIST_QUERY" then
        cmd = "list"
    elseif action == "QUERY" then
        cmd = "status"
    end

    -- 1. Try Addon wire message if in group/raid
    local payload = action .. "\t" .. tostring(guid or 0) .. "\t" .. tostring(level or 0)
    if rate then
        payload = payload .. "\t" .. tostring(rate)
    end

    local inGroup = (GetNumPartyMembers() > 0) or (GetNumRaidMembers() > 0)
    if inGroup then
        SendAddonMessage(ADDON_PREFIX, payload, "PARTY")
    end

    -- 2. Send slash command directly if runChatCmd is true OR if solo (not grouped)
    -- This guarantees immediate server reception and execution in ALL situations!
    if cmd ~= "" and (runChatCmd or not inGroup) then
        SendChatMessage(".botxplock " .. cmd, "SAY")
    end
end

-- -----------------------------------------------------------------------------
-- 4. UI View Updaters
-- -----------------------------------------------------------------------------
local function UpdateTargetDisplay()
    local name = UnitName("target")
    if not name or not UnitIsPlayer("target") or not UnitIsFriend("player", "target") then
        currentTargetGuid = 0
        currentTargetStatus = 0
        Turtle_BotXpLock_TargetName:SetText("No Bot Selected")
        Turtle_BotXpLock_TargetInfo:SetText("Target a friendly companion bot or party member")
        Turtle_BotXpLock_StatusText:SetText("XP STATUS: NO TARGET")
        Turtle_BotXpLock_StatusBadge:SetBackdropColor(0.12, 0.12, 0.15, 0.85)
        Turtle_BotXpLock_FreezeButton:Disable()
        Turtle_BotXpLock_UnlockButton:Disable()
        for i = 19, 59, 10 do
            local btn = getglobal("Turtle_BotXpLock_Cap" .. i)
            if btn then btn:Disable() end
        end
        return
    end

    local level = UnitLevel("target")
    local class, _ = UnitClass("target")
    Turtle_BotXpLock_TargetName:SetText("|cffffd100" .. name .. "|r")
    Turtle_BotXpLock_TargetInfo:SetText("Level " .. tostring(level) .. " " .. (class or "Player"))

    Turtle_BotXpLock_FreezeButton:Enable()
    Turtle_BotXpLock_UnlockButton:Enable()
    for i = 19, 59, 10 do
        local btn = getglobal("Turtle_BotXpLock_Cap" .. i)
        if btn then btn:Enable() end
    end

    -- Query fresh status silently from server (works whether solo or grouped)
    SendModuleMessage("QUERY", 0, 0, nil, false)
end

local function ApplyStatusData(guid, name, level, curXP, nextXP, isLocked, lockLevel, xpRate)
    local targetName = UnitName("target")
    local rateVal = xpRate or 1.0

    if targetName and (targetName == name or guid == currentTargetGuid) then
        currentTargetGuid = guid
        if isLocked == 1 or rateVal <= 0 then
            if lockLevel > 0 and level < lockLevel then
                currentTargetStatus = 2 -- Capped
                Turtle_BotXpLock_StatusText:SetText("|cffffa500CAPPED AT LVL " .. tostring(lockLevel) .. " (RATE: " .. tostring(rateVal) .. "x)|r")
                Turtle_BotXpLock_StatusBadge:SetBackdropColor(0.35, 0.25, 0.05, 0.90)
            else
                currentTargetStatus = 1 -- Locked
                local lockDisp = (lockLevel > 0) and lockLevel or level
                Turtle_BotXpLock_StatusText:SetText("|cffff2020LOCKED AT LVL " .. tostring(lockDisp) .. " (0x XP)|r")
                Turtle_BotXpLock_StatusBadge:SetBackdropColor(0.40, 0.08, 0.08, 0.90)
            end
        else
            currentTargetStatus = 0 -- Unlocked
            if rateVal ~= 1.0 then
                Turtle_BotXpLock_StatusText:SetText("|cff00ff00XP ACTIVE (RATE: |cffffd100" .. tostring(rateVal) .. "x|cff00ff00)|r")
            else
                Turtle_BotXpLock_StatusText:SetText("|cff00ff00XP ACTIVE (NORMAL 1.0x)|r")
            end
            Turtle_BotXpLock_StatusBadge:SetBackdropColor(0.08, 0.35, 0.08, 0.90)
        end
    end

    -- Update party rows and button states
    local numParty = GetNumPartyMembers()
    for i = 1, 4 do
        local row = getglobal("Turtle_BotXpLock_PartyRow" .. i)
        if row and i <= numParty then
            local pName = UnitName("party" .. i)
            if pName and pName == name then
                partyBotStatus[i] = {
                    isLocked = isLocked,
                    lockLevel = lockLevel,
                    xpRate = rateVal
                }

                local statusStr = "|cff00ff00" .. tostring(rateVal) .. "x|r"
                local toggleBtn = getglobal("Turtle_BotXpLock_PartyRow" .. i .. "ToggleBtn")

                if isLocked == 1 or rateVal <= 0 then
                    if lockLevel > 0 and level < lockLevel then
                        statusStr = "|cffffa500CAP " .. tostring(lockLevel) .. "|r"
                    else
                        statusStr = "|cffff2020LOCKED|r"
                    end
                    if toggleBtn then toggleBtn:SetText("Unlock") end
                else
                    if toggleBtn then toggleBtn:SetText("Freeze") end
                end

                local statusText = getglobal("Turtle_BotXpLock_PartyRow" .. i .. "Status")
                if statusText then statusText:SetText(statusStr) end
            end
        end
    end
end

local function UpdatePartyRoster()
    local numParty = GetNumPartyMembers()
    for i = 1, 4 do
        local row = getglobal("Turtle_BotXpLock_PartyRow" .. i)
        if row then
            if i <= numParty then
                local pName = UnitName("party" .. i)
                local pLevel = UnitLevel("party" .. i)

                row:Show()
                local nameText = getglobal("Turtle_BotXpLock_PartyRow" .. i .. "Name")
                if nameText then
                    nameText:SetText(pName or "Party" .. i)
                end

                local toggleBtn = getglobal("Turtle_BotXpLock_PartyRow" .. i .. "ToggleBtn")
                local pData = partyBotStatus[i]
                if pData and (pData.isLocked == 1 or pData.xpRate <= 0) then
                    if toggleBtn then toggleBtn:SetText("Unlock") end
                else
                    if toggleBtn then toggleBtn:SetText("Freeze") end
                end

                local statusText = getglobal("Turtle_BotXpLock_PartyRow" .. i .. "Status")
                if statusText and (not pData) then
                    statusText:SetText("Lvl " .. tostring(pLevel))
                end
            else
                row:Hide()
            end
        end
    end
end

-- -----------------------------------------------------------------------------
-- 5. Configured Bots Registry Panel Display (Reads SQL / Server Cache)
-- -----------------------------------------------------------------------------
local function UpdateRegistryDisplay()
    local total = table.getn(registryBots)
    Turtle_BotXpLock_RegistryCount:SetText("Total: " .. tostring(total) .. " bots configured")

    local maxPages = math.max(1, math.ceil(total / ROWS_PER_PAGE))
    if registryPage > maxPages then registryPage = maxPages end
    if registryPage < 1 then registryPage = 1 end

    Turtle_BotXpLock_RegPageText:SetText("Page " .. tostring(registryPage) .. " / " .. tostring(maxPages))

    local startIdx = (registryPage - 1) * ROWS_PER_PAGE + 1
    for r = 1, ROWS_PER_PAGE do
        local row = getglobal("Turtle_BotXpLock_RegRow" .. r)
        local botIdx = startIdx + (r - 1)
        if botIdx <= total then
            local entry = registryBots[botIdx]
            row:Show()
            row:SetID(botIdx)

            local nameTxt = getglobal("Turtle_BotXpLock_RegRow" .. r .. "Name")
            local lvlTxt = getglobal("Turtle_BotXpLock_RegRow" .. r .. "Level")
            local statusTxt = getglobal("Turtle_BotXpLock_RegRow" .. r .. "Status")

            if nameTxt then nameTxt:SetText(entry.name) end
            if lvlTxt then lvlTxt:SetText("Lvl " .. tostring(entry.level)) end

            local statusStr = "|cff00ff00" .. tostring(entry.xpRate) .. "x|r"
            if entry.locked == 1 or entry.xpRate <= 0 then
                if entry.lockLevel > 0 and entry.level < entry.lockLevel then
                    statusStr = "|cffffa500CAP " .. tostring(entry.lockLevel) .. "|r"
                else
                    statusStr = "|cffff2020LOCKED|r"
                end
            end
            if statusTxt then statusTxt:SetText(statusStr) end
        else
            row:Hide()
        end
    end
end

function TurtleBotXpLock_OnRegistryPage(delta)
    registryPage = registryPage + delta
    UpdateRegistryDisplay()
end

function TurtleBotXpLock_OnRegistryRowSelect(idx)
    local entry = registryBots[idx]
    if not entry then return end
    TargetByName(entry.name, true)
    UpdateTargetDisplay()
end

function TurtleBotXpLock_ToggleRegistry()
    if Turtle_BotXpLock_RegistryFrame:IsShown() then
        Turtle_BotXpLock_RegistryFrame:Hide()
        Turtle_BotXpLock_ToggleRegistryButton:SetText("All Configured Bots >>")
    else
        Turtle_BotXpLock_RegistryFrame:Show()
        Turtle_BotXpLock_ToggleRegistryButton:SetText("<< Hide Registry")
        SendModuleMessage("LIST_QUERY", 0, 0, nil, true)
    end
end

-- -----------------------------------------------------------------------------
-- 6. Click Handlers
-- -----------------------------------------------------------------------------
function TurtleBotXpLock_OnFreezeClick()
    local name = UnitName("target")
    if not name then return end
    SendModuleMessage("LOCK", 0, 0, nil, true)
    currentTargetStatus = 1
    Turtle_BotXpLock_StatusText:SetText("|cffff2020LOCKED (0x XP)|r")
    Turtle_BotXpLock_StatusBadge:SetBackdropColor(0.40, 0.08, 0.08, 0.90)
    DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Locked XP for |cffffd100" .. name .. "|r.")
end

function TurtleBotXpLock_OnUnlockClick()
    local name = UnitName("target")
    if not name then return end
    SendModuleMessage("UNLOCK", 0, 0, nil, true)
    currentTargetStatus = 0
    Turtle_BotXpLock_StatusText:SetText("|cff00ff00XP ACTIVE (NORMAL 1.0x)|r")
    Turtle_BotXpLock_StatusBadge:SetBackdropColor(0.08, 0.35, 0.08, 0.90)
    DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Unlocked XP for |cffffd100" .. name .. "|r.")
end

function TurtleBotXpLock_OnCapClick(level)
    local name = UnitName("target")
    if not name then return end
    SendModuleMessage("LOCK", 0, level, nil, true)
    currentTargetStatus = 2
    Turtle_BotXpLock_StatusText:SetText("|cffffa500CAPPED AT LVL " .. tostring(level) .. "|r")
    Turtle_BotXpLock_StatusBadge:SetBackdropColor(0.35, 0.25, 0.05, 0.90)
    DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Set Level " .. tostring(level) .. " cap for |cffffd100" .. name .. "|r.")
end

function TurtleBotXpLock_OnCustomCapSet()
    local name = UnitName("target")
    if not name then
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Target a companion bot first to set a custom cap.")
        return
    end
    local valStr = Turtle_BotXpLock_CustomCapBox:GetText()
    local level = tonumber(valStr)
    if not level or level < 1 or level > 60 then
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Please enter a valid level cap between 1 and 60.")
        return
    end
    Turtle_BotXpLock_CustomCapBox:ClearFocus()
    SendModuleMessage("LOCK", 0, level, nil, true)
    currentTargetStatus = 2
    Turtle_BotXpLock_StatusText:SetText("|cffffa500CAPPED AT LVL " .. tostring(level) .. "|r")
    Turtle_BotXpLock_StatusBadge:SetBackdropColor(0.35, 0.25, 0.05, 0.90)
    DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Custom cap of Level " .. tostring(level) .. " applied to |cffffd100" .. name .. "|r.")
end

function TurtleBotXpLock_OnRateClick(rate)
    local name = UnitName("target")
    if not name then return end
    SendModuleMessage("RATE", 0, 0, rate, true)
    Turtle_BotXpLock_StatusText:SetText("|cff00ff00XP ACTIVE (RATE: |cffffd100" .. tostring(rate) .. "x|cff00ff00)|r")
    DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Set XP rate to " .. tostring(rate) .. "x for |cffffd100" .. name .. "|r.")
end

-- Toggles Party Member XP between Freeze and Unlock based on active status
function TurtleBotXpLock_OnPartyRowToggle(partyIndex)
    local unit = "party" .. tostring(partyIndex)
    local name = UnitName(unit)
    if not name then return end

    TargetUnit(unit)
    local pData = partyBotStatus[partyIndex]
    local isCurrentlyLocked = pData and (pData.isLocked == 1 or pData.xpRate <= 0)

    if isCurrentlyLocked then
        SendModuleMessage("UNLOCK", 0, 0, nil, true)
        if pData then pData.isLocked = 0 end
        local toggleBtn = getglobal("Turtle_BotXpLock_PartyRow" .. partyIndex .. "ToggleBtn")
        if toggleBtn then toggleBtn:SetText("Freeze") end
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Unlocked XP for |cffffd100" .. name .. "|r.")
    else
        SendModuleMessage("LOCK", 0, 0, nil, true)
        if pData then pData.isLocked = 1 end
        local toggleBtn = getglobal("Turtle_BotXpLock_PartyRow" .. partyIndex .. "ToggleBtn")
        if toggleBtn then toggleBtn:SetText("Unlock") end
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Locked XP for |cffffd100" .. name .. "|r.")
    end
end

function TurtleBotXpLock_Refresh()
    UpdateTargetDisplay()
    UpdatePartyRoster()
    SendModuleMessage("ROSTER_QUERY", 0, 0, nil, false)
    if Turtle_BotXpLock_RegistryFrame:IsShown() then
        SendModuleMessage("LIST_QUERY", 0, 0, nil, true)
    end
end

-- -----------------------------------------------------------------------------
-- 7. Minimap Button (31x31 Vanilla Standard Circular Button)
-- Left-Click: Quick Toggle Target Bot XP Lock (Freeze or Unlock)
-- Right-Click / Shift-Click: Open Controller Panel
-- -----------------------------------------------------------------------------
local minimapBtn = CreateFrame("Button", "TurtleBotXpLockMinimapButton", Minimap)
minimapBtn:SetFrameStrata("MEDIUM")
minimapBtn:SetFrameLevel(8)
minimapBtn:SetWidth(31)
minimapBtn:SetHeight(31)
minimapBtn:RegisterForClicks("LeftButtonUp", "RightButtonUp")
minimapBtn:RegisterForDrag("LeftButton")
minimapBtn:SetHighlightTexture("Interface\\Minimap\\UI-Minimap-ZoomButton-Highlight")

local mmOverlay = minimapBtn:CreateTexture(nil, "OVERLAY")
mmOverlay:SetWidth(53)
mmOverlay:SetHeight(53)
mmOverlay:SetTexture("Interface\Minimap\MiniMap-TrackingBorder")
mmOverlay:SetPoint("TOPLEFT", minimapBtn, "TOPLEFT", 0, 0)

local mmIcon = minimapBtn:CreateTexture(nil, "BACKGROUND")
mmIcon:SetWidth(20)
mmIcon:SetHeight(20)
mmIcon:SetTexture("Interface\\Icons\\Spell_Nature_TimeStop")
mmIcon:SetPoint("CENTER", minimapBtn, "CENTER", 0, 1)
mmIcon:SetTexCoord(0.07, 0.93, 0.07, 0.93)

local function UpdateMinimapPosition()
    local pos = 225
    if Turtle_BotXpLock_DB and Turtle_BotXpLock_DB.minimapPos then
        pos = Turtle_BotXpLock_DB.minimapPos
    end
    local angle = math.rad(pos)
    minimapBtn:ClearAllPoints()
    minimapBtn:SetPoint("CENTER", Minimap, "CENTER", 80 * math.cos(angle), 80 * math.sin(angle))
end

minimapBtn:SetScript("OnDragStart", function()
    this:SetScript("OnUpdate", function()
        local mx, my = Minimap:GetCenter()
        local scale = Minimap:GetEffectiveScale()
        local px, py = GetCursorPosition()
        px, py = px / scale, py / scale
        if not Turtle_BotXpLock_DB then Turtle_BotXpLock_DB = {} end
        Turtle_BotXpLock_DB.minimapPos = math.deg(math.atan2(py - my, px - mx))
        UpdateMinimapPosition()
    end)
end)

minimapBtn:SetScript("OnDragStop", function()
    this:SetScript("OnUpdate", nil)
end)

minimapBtn:SetScript("OnClick", function()
    if arg1 == "RightButton" or (IsShiftKeyDown and IsShiftKeyDown()) or (IsControlKeyDown and IsControlKeyDown()) or (IsAltKeyDown and IsAltKeyDown()) then
        if Turtle_BotXpLockFrame:IsShown() then
            HideUIPanel(Turtle_BotXpLockFrame)
        else
            ShowUIPanel(Turtle_BotXpLockFrame)
            TurtleBotXpLock_Refresh()
        end
    else
        if UnitExists("target") and UnitIsPlayer("target") and UnitIsFriend("player", "target") then
            -- Quick Toggle target bot
            if currentTargetStatus == 1 or currentTargetStatus == 2 then
                SendModuleMessage("UNLOCK", 0, 0, nil, true)
                currentTargetStatus = 0
                DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Unlocked XP for |cffffd100" .. UnitName("target") .. "|r.")
            else
                SendModuleMessage("LOCK", 0, 0, nil, true)
                currentTargetStatus = 1
                DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Locked XP for |cffffd100" .. UnitName("target") .. "|r.")
            end
        else
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Target a companion bot to toggle XP lock, or Right-Click / Shift-Click to open Controller window.")
        end
    end
end)

minimapBtn:SetScript("OnEnter", function()
    GameTooltip:SetOwner(this, "ANCHOR_LEFT")
    GameTooltip:ClearLines()
    GameTooltip:AddLine("Bot XP Controller", 1.0, 0.82, 0.0)
    GameTooltip:AddLine("|cffffffffLeft-Click:|r Quick-Toggle Target Bot XP Lock", 0.9, 0.9, 0.9)
    GameTooltip:AddLine("|cffffffffRight-Click / Shift-Click:|r Open Controller Panel", 0.8, 0.8, 0.8)
    GameTooltip:AddLine("|cffffffffDrag:|r Move Minimap Icon", 0.6, 0.6, 0.6)
    GameTooltip:Show()
end)

minimapBtn:SetScript("OnLeave", function()
    GameTooltip:Hide()
end)

-- -----------------------------------------------------------------------------
-- 8. Core Event Listener
-- -----------------------------------------------------------------------------
local eventFrame = CreateFrame("Frame", "Turtle_BotXpLock_EventFrame")
eventFrame:RegisterEvent("ADDON_LOADED")
eventFrame:RegisterEvent("PLAYER_TARGET_CHANGED")
eventFrame:RegisterEvent("PARTY_MEMBERS_CHANGED")
eventFrame:RegisterEvent("CHAT_MSG_ADDON")

eventFrame:SetScript("OnEvent", function()
    if event == "ADDON_LOADED" then
        if arg1 == "Turtle_BotXpLock" then
            if not Turtle_BotXpLock_DB then
                Turtle_BotXpLock_DB = { minimapPos = 225 }
            end
            UpdateMinimapPosition()
        end
    elseif event == "PLAYER_TARGET_CHANGED" then
        if Turtle_BotXpLockFrame:IsShown() then
            UpdateTargetDisplay()
        elseif UnitExists("target") and UnitIsPlayer("target") and UnitIsFriend("player", "target") then
            -- Silent background query so minimap button toggle knows status immediately
            SendModuleMessage("QUERY", 0, 0, nil, false)
        end
    elseif event == "PARTY_MEMBERS_CHANGED" then
        if Turtle_BotXpLockFrame:IsShown() then
            UpdatePartyRoster()
        end
    elseif event == "CHAT_MSG_ADDON" then
        if arg1 == ADDON_PREFIX then
            local tokens = SplitString(arg2, "\t")
            local nTokens = table.getn(tokens)
            if nTokens >= 8 and tokens[1] == "STATUS" then
                local guid = tonumber(tokens[2]) or 0
                local name = tokens[3]
                local level = tonumber(tokens[4]) or 0
                local curXP = tonumber(tokens[5]) or 0
                local nextXP = tonumber(tokens[6]) or 0
                local isLocked = tonumber(tokens[7]) or 0
                local lockLevel = tonumber(tokens[8]) or 0
                local xpRate = 1.0
                if nTokens >= 9 then
                    xpRate = tonumber(tokens[9]) or 1.0
                end

                ApplyStatusData(guid, name, level, curXP, nextXP, isLocked, lockLevel, xpRate)
            elseif tokens[1] == "LIST_START" then
                registryBots = {}
            elseif tokens[1] == "LIST_ITEM" and nTokens >= 7 then
                local item = {
                    guid = tonumber(tokens[2]) or 0,
                    name = tokens[3],
                    level = tonumber(tokens[4]) or 0,
                    locked = tonumber(tokens[5]) or 0,
                    lockLevel = tonumber(tokens[6]) or 0,
                    xpRate = tonumber(tokens[7]) or 1.0
                }
                table.insert(registryBots, item)
            elseif tokens[1] == "LIST_END" then
                UpdateRegistryDisplay()
            end
        end
    end
end)

function TurtleBotXpLock_OnLoad()
    for i = 1, 4 do
        local row = getglobal("Turtle_BotXpLock_PartyRow" .. i)
        if row then row:Hide() end
    end
    for r = 1, ROWS_PER_PAGE do
        local row = getglobal("Turtle_BotXpLock_RegRow" .. r)
        if row then row:Hide() end
    end
    UpdatePartyRoster()
end

-- -----------------------------------------------------------------------------
-- 9. Chat Slash Commands
-- -----------------------------------------------------------------------------
SLASH_TURTLEBOTXPLOCK1 = "/botxplock"
SLASH_TURTLEBOTXPLOCK2 = "/xplock"

SlashCmdList["TURTLEBOTXPLOCK"] = function(msg)
    if not msg or msg == "" then
        if Turtle_BotXpLockFrame:IsShown() then
            HideUIPanel(Turtle_BotXpLockFrame)
        else
            ShowUIPanel(Turtle_BotXpLockFrame)
            TurtleBotXpLock_Refresh()
        end
        return
    end

    local tokens = SplitString(msg, " ")
    local cmd = string.lower(tokens[1] or "")

    if cmd == "lock" or cmd == "freeze" then
        local cap = tonumber(tokens[2]) or 0
        SendModuleMessage("LOCK", 0, cap, nil, true)
    elseif cmd == "unlock" or cmd == "unfreeze" then
        SendModuleMessage("UNLOCK", 0, 0, nil, true)
    elseif cmd == "toggle" then
        SendModuleMessage("TOGGLE", 0, 0, nil, true)
    elseif cmd == "rate" then
        local rate = tonumber(tokens[2]) or 1.0
        SendModuleMessage("RATE", 0, 0, rate, true)
    elseif cmd == "status" or cmd == "info" then
        SendModuleMessage("QUERY", 0, 0, nil, false)
    elseif cmd == "list" then
        TurtleBotXpLock_ToggleRegistry()
    else
        DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[BotXpLock]|r Commands:")
        DEFAULT_CHAT_FRAME:AddMessage("  |cffffd100/xplock|r - Toggle Controller Panel")
        DEFAULT_CHAT_FRAME:AddMessage("  |cffffd100/xplock toggle|r - Toggle XP lock (freeze/unlock)")
        DEFAULT_CHAT_FRAME:AddMessage("  |cffffd100/xplock lock [capLevel]|r - Lock target bot (e.g. /xplock lock 19)")
        DEFAULT_CHAT_FRAME:AddMessage("  |cffffd100/xplock rate <mult>|r - Set XP rate (e.g. /xplock rate 0.5)")
        DEFAULT_CHAT_FRAME:AddMessage("  |cffffd100/xplock unlock|r - Unlock target bot (1.0x)")
        DEFAULT_CHAT_FRAME:AddMessage("  |cffffd100/xplock list|r - Open All Configured Bots Registry Panel")
    end
end
