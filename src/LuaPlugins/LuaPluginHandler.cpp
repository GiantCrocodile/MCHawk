#include "LuaPluginHandler.hpp"

#include "LuaPluginAPI.hpp"

#include <iostream>

std::array<boost::signals2::signal<void(Client*, luabridge::LuaRef)>, kEventTypeEnd> LuaPluginHandler::signalMap;

lua_State* L;

LuaPluginHandler::LuaPluginHandler()
{
	L = luaL_newstate();
	luaL_openlibs(L);

	LuaServer::Init(L);

	if (luaL_dofile(L, "plugins/core/init.lua") != 0)
		std::cerr << "Failed to load init.lua" << std::endl;
}

LuaPluginHandler::~LuaPluginHandler()
{
	for (auto& obj : m_plugins)
		delete obj;
}

void LuaPluginHandler::AddPlugin(LuaPlugin* plugin)
{
	m_plugins.push_back(plugin);
	plugin->Init();
}

void LuaPluginHandler::LoadPlugin(std::string filename)
{
	LuaPlugin* plugin = new LuaPlugin;
	plugin->LoadScript(L, filename);
	AddPlugin(plugin);

	auto table = make_luatable();

	table["name"] = plugin->GetName();

	TriggerEvent(EventType::kOnPluginLoaded, nullptr, table);
}

void LuaPluginHandler::RegisterEvent(int type, luabridge::LuaRef func)
{
	if (func.isFunction()) {
		try {
			signalMap[type].connect(boost::bind((std::function<void(Client*, luabridge::LuaRef)>)func, _1, _2));
		} catch (luabridge::LuaException const& e) {
			std::cerr << "LuaException: " << e.what() << std::endl;
		}
	}
}

void LuaPluginHandler::TriggerEvent(int type, Client* client, luabridge::LuaRef table)
{
	try {
		signalMap[type](client, table);
	} catch (luabridge::LuaException const& e) {
		std::cerr << "LuaException: " << e.what() << std::endl;
	}
}
