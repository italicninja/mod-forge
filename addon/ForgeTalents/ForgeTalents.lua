-- ForgeTalents (mod-forge): shows talent ranks from gear ("+1 to Improved Fireball") in the talent window.
-- The server (mod-forge, Forge.TalentAffixes.Enable) sends them as an addon whisper to the player:
-- prefix FORGE_TB, "26=1,30=2" (Talent.dbc id = ranks, empty = none), at login and whenever they change.
-- The addon asks again with "?" when it loads (a /reload). Only the shown numbers change: spending points works
-- as always, the server does the rest.

local PREFIX = "FORGE_TB"
local GREEN = "|cff1eff00"
local bonus = {}

local function TalentId(tab, index, inspect, pet, group)
    local link = GetTalentLink(tab, index, inspect, pet, group)
    return link and tonumber(link:match("Htalent:(%d+)"))
end

-- Bonuses apply to the player's own active spec only.
local function IsActive(inspect, pet, group)
    return not inspect and not pet and (group or GetActiveTalentGroup()) == GetActiveTalentGroup()
end

local function UpdateFrame(frame)
    if not frame or not IsActive(frame.inspect, frame.pet, frame.talentGroup) or not next(bonus) then
        return
    end
    local tab = PanelTemplates_GetSelectedTab(frame)
    if not tab then
        return
    end
    local name = frame:GetName()
    for i = 1, GetNumTalents(tab) do
        local id = TalentId(tab, i, false, false, frame.talentGroup)
        local extra = id and bonus[id]
        if extra then
            local _, _, _, _, rank = GetTalentInfo(tab, i, false, false, frame.talentGroup)
            local text = _G[name .. "Talent" .. i .. "Rank"]
            local border = _G[name .. "Talent" .. i .. "RankBorder"]
            if text then
                text:SetText(GREEN .. (rank + extra) .. "|r")
                text:Show()
            end
            if border then
                border:Show()
            end
        end
    end
end

local function Refresh()
    if PlayerTalentFrame and PlayerTalentFrame:IsShown() and TalentFrame_Update then
        TalentFrame_Update(PlayerTalentFrame)
    end
end

local hooked = false
local function Hook()
    if hooked or not TalentFrame_Update then
        return
    end
    hooksecurefunc("TalentFrame_Update", UpdateFrame)
    hooked = true
end

hooksecurefunc(GameTooltip, "SetTalent", function(self, tab, index, inspect, pet, group)
    if not IsActive(inspect, pet, group) then
        return
    end
    local id = TalentId(tab, index, inspect, pet, group)
    local extra = id and bonus[id]
    if not extra then
        return
    end
    local _, _, _, _, rank, maxRank = GetTalentInfo(tab, index, inspect, pet, group)
    self:AddLine(GREEN .. format("+%d from gear: works at rank %d/%d", extra, rank + extra, maxRank) .. "|r", nil, nil, nil, true)
    self:Show()
end)

local events = CreateFrame("Frame")
events:RegisterEvent("ADDON_LOADED")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:RegisterEvent("CHAT_MSG_ADDON")
events:SetScript("OnEvent", function(self, event, arg1, arg2, arg3, arg4)
    if event == "CHAT_MSG_ADDON" then
        if arg1 ~= PREFIX or arg4 ~= UnitName("player") then
            return
        end
        wipe(bonus)
        for id, ranks in arg2:gmatch("(%d+)=(%d+)") do
            bonus[tonumber(id)] = tonumber(ranks)
        end
        Refresh()
    elseif event == "ADDON_LOADED" then
        Hook() -- TalentFrame_Update may come with Blizzard_TalentUI (load on demand)
    elseif event == "PLAYER_ENTERING_WORLD" then
        Hook()
        SendAddonMessage(PREFIX, "?", "WHISPER", UnitName("player"))
    end
end)
Hook()
