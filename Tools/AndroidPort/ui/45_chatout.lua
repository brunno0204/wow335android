-- Android port: sending chat (CMSG_MESSAGECHAT) and emotes (CMSG_SEND_TEXT_EMOTE).
function SendChatMessage(message, chatType, language, target)
  if type(message) ~= "string" or message == "" then return end
  chatType = strupper(chatType or "SAY")
  local index = GetChatTypeIndex(chatType)
  if index == 0 then return end
  local wire = index - 1               -- ChatMsg values follow the chat type table order
  local languageId = __AndroidLanguageId(language)
  if chatType == "WHISPER" then
    __AndroidSend(OP.CMSG_MESSAGECHAT, "iiss", wire, languageId, tostring(target or ""), message)
  elseif chatType == "CHANNEL" then
    local _, name = GetChannelName(target)
    __AndroidSend(OP.CMSG_MESSAGECHAT, "iiss", wire, languageId, name or tostring(target or ""), message)
  else
    __AndroidSend(OP.CMSG_MESSAGECHAT, "iis", wire, languageId, message)
  end
end
function SendAddonMessage() end
function RandomRoll(low, high) __AndroidSend(OP.MSG_RANDOM_ROLL, "ii", tonumber(low) or 1, tonumber(high) or 100) end

local emotes
function DoEmote(token, unit)
  if type(token) ~= "string" then return end
  if not emotes then
    emotes = {}
    for i = 1, __AndroidDbcCount("EmotesText") do
      local id, name = __AndroidDbcRow("EmotesText", i, "id", "name")
      if name then emotes[strupper(name)] = id end
    end
  end
  local id = emotes[strupper(token)]
  if not id then return end
  local guid = UnitGUID(unit or "target") or "0x0000000000000000"
  __AndroidSend(OP.CMSG_SEND_TEXT_EMOTE, "iil", id, 0xFFFFFFFF, guid)
end
