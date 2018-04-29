/*
 * Vexyl - March/April, 2017
 * TODO:
 *   Only send position updates if player moved--send on a regular basis, not when received
 *   Line wrapping should keep track of color codes and break on words
 *   Leaf decay/grass growth/fluids
 *   ClientStreams/Various sfml code should be abstracted away in a TcpStream class
 *   Implement singletons correctly (or find a way around using them)
 *   Opcodes should be an enum, not a chain of #defines
 *   Packet queue is inefficient and leaks memory (needs copy constructor?)
 *   Separate authed clients from pre-auth clients (two separate vectors would work)
 *   Learn about/use smart pointers
 *   Use Google C++ style guidelines (I just want to)
 * Eventually (ideally):
 *   Events/Scripts (boost::signals/Lua); use these for commands
 *   Have  a /set <setting name> <argument> command instead of command for each
 *   Improve Logger (e.g., multiple file support; replace with boost logger?)
 *   Support classic protocol extension
 *   Upload to GitHub/Use cmake
 *   Strive for platform independence and have a Visual Studio project file with dependencies
 */

#include "Server.hpp"

#ifdef __linux__
	#include <arpa/inet.h>
#elif _WIN32
	#include <winsock2.h>
#endif

#include <openssl/md5.h>

#include <iostream>
#include <stdexcept>

#include <zlib.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "Network/ClientStream.hpp"
#include "Utils/Logger.hpp"
#include "Utils/Utils.hpp"


#include "Commands/HelpCommand.hpp"
#include "Commands/TeleportCommand.hpp"
#include "Commands/SummonCommand.hpp"
#include "Commands/BillNyeCommand.hpp"
#include "Commands/EmoteCommand.hpp"
#include "Commands/PmCommand.hpp"
#include "Commands/OpCommand.hpp"
#include "Commands/KickCommand.hpp"
#include "Commands/SaveCommand.hpp"
#include "Commands/AliasCommand.hpp"
#include "Commands/WhoCommand.hpp"
#include "Commands/WhoIsCommand.hpp"
#include "Commands/GotoCommand.hpp"
#include "Commands/WorldCommand.hpp"

#include "LuaPlugins/LuaPluginAPI.hpp"

Server* Server::m_thisPtr = nullptr;

Server::Server()
{
	m_port = 25565;
	m_version = 0x07;
	m_numClients = 0;
	m_maxClients = 8;
	m_serverHeartbeat = false;
	m_serverPublic = false;
	m_serverVerifyNames = false;
}

Server::~Server()
{
	for (auto& obj : m_clients) {
		delete obj->stream.socket;
		delete obj;
	}

	for (auto& obj : m_worlds)
		delete obj.second;
}

void Server::Init()
{
	// Turn off SFML errors
	sf::err().rdbuf(nullptr);

	bool debug = true;

	try {
		boost::property_tree::ptree pt;
		boost::property_tree::ini_parser::read_ini("config.ini", pt);

		m_serverName = pt.get<std::string>("Server.name");
		m_serverMotd = pt.get<std::string>("Server.motd");
		m_port = pt.get<int>("Server.port");
		m_serverHeartbeat = pt.get<bool>("Server.heartbeat");
		m_serverPublic = pt.get<bool>("Server.public");
		m_maxClients = pt.get<int>("Server.max_users");
		m_serverVerifyNames = pt.get<bool>("Server.verify_names");
		debug = pt.get<bool>("Server.debug");
	} catch (std::runtime_error& e) {
		LOG(LogLevel::kWarning, "%s", e.what());
	}

	if (!debug)
		Logger::GetLogger()->SetVerbosityLevel(VerbosityLevel::kQuiet);

	if (m_listener.listen(m_port) != sf::Socket::Done) {
		LOG(LogLevel::kError, "Failed to listen on port %d", m_port);
		exit(1);
	}

	m_listener.setBlocking(false);

	LOG(LogLevel::kInfo, "Server initialized and listening on port %d", m_port);

	if (!m_serverVerifyNames)
		LOG(LogLevel::kWarning, "Verify names is turned off! This is NOT secure and disabling it should only be necessary during server tests. After that, TURN IT BACK ON.");

	m_salt = Utils::GetRandomSalt();

	SendHeartbeat();

	// Scrap = flat world that doesn't autosave
	World* w = new World("scrap");

	w->GetMap().GenerateFlatMap(256, 16, 256);
	w->SetSpawnPosition(Position(512/2*32+51, 64/2*32+51, 512/2*32+51));
	w->SetOption("autosave", "false");
	w->SetActive(true);

	AddWorld(w);

	// Load all worlds from config files
	for (boost::filesystem::directory_iterator itr("worlds/"); itr != boost::filesystem::directory_iterator(); ++itr) {
		if (boost::filesystem::is_regular_file(itr->status())) {
			std::string filename = itr->path().filename().string();

			try {
				boost::property_tree::ptree pt;
				boost::property_tree::ini_parser::read_ini("worlds/" + filename, pt);

				std::string name = pt.get<std::string>("World.name");
				std::string map = pt.get<std::string>("World.map");

				short x_size = pt.get<short>("Size.x");
				short y_size = pt.get<short>("Size.y");
				short z_size = pt.get<short>("Size.z");

				short sx = pt.get<short>("Spawn.x");
				short sy = pt.get<short>("Spawn.y");
				short sz = pt.get<short>("Spawn.z");

				std::string autosave = pt.get<std::string>("Options.autosave");
				std::string build = pt.get<std::string>("Options.build");
				std::string autoload = pt.get<std::string>("Options.autoload");

				World* world = new World(name);
				world->GetMap().SetDimensions(x_size, y_size, z_size);
				world->GetMap().SetFilename("worlds/" + map);
				world->SetSpawnPosition(Position(sx, sy, sz));
				world->SetOption("autosave", autosave);
				world->SetOption("build", build);
				world->SetOption("autoload", autoload);

				if (autoload == "true")
					world->Load();
				else
					LOG(LogLevel::kDebug, "Unloaded world %s", name.c_str());

				AddWorld(world);
			} catch (std::runtime_error& e) {
				LOG(LogLevel::kWarning, "%s", e.what());
			}
		}
	}

	m_commandHandler.Register("help", new HelpCommand, "h cmds cmd");
	m_commandHandler.Register("tp", new TeleportCommand);
	m_commandHandler.Register("summon", new SummonCommand);
	m_commandHandler.Register("billnye", new BillNyeCommand);
	m_commandHandler.Register("me", new EmoteCommand, "emote");
	m_commandHandler.Register("pm", new PmCommand, "msg");
	m_commandHandler.Register("op", new OpCommand);
	m_commandHandler.Register("kick", new KickCommand);
	m_commandHandler.Register("save", new SaveCommand);
	m_commandHandler.Register("alias", new AliasCommand, "name");
	m_commandHandler.Register("who", new WhoCommand, "players list");
	m_commandHandler.Register("whois", new WhoIsCommand, "info");
	m_commandHandler.Register("goto", new GotoCommand, "go g");
	m_commandHandler.Register("world", new WorldCommand, "w map");

	m_pluginHandler.LoadPlugin("plugins/core/init.lua"); // Load this first

	// Core scripts may load other plugins, load them now
	m_pluginHandler.FlushPluginQueue();

	// Load all plugins in folder
	for (boost::filesystem::directory_iterator itr("plugins"); itr != boost::filesystem::directory_iterator(); ++itr) {
		if (boost::filesystem::is_directory(itr->status())) {
			std::string path = itr->path().generic_string();
			if (path != "plugins/core") {
				std::string filename = path + "/init.lua";
				if (boost::filesystem::exists(filename)) {
					m_pluginHandler.LoadPlugin(filename);
				}
			}
		}
	}

	// In case a script loaded another plugin while being loaded
	m_pluginHandler.FlushPluginQueue();
}

void Server::OnConnect(sf::TcpSocket *sock)
{
	Client* client = new Client();

	client->active = true;
	client->authed = false;
	client->stream.socket = sock;

	m_clients.push_back(client);

	LOG(LogLevel::kDebug, "Client connected (%s)", client->GetIpString().c_str());
}

void Server::OnAuth(Client* client, struct cauthp clientAuth)
{
	std::string name((char*)clientAuth.name, 0, sizeof(clientAuth.name));
	std::string key((char*)clientAuth.key, 0, sizeof(clientAuth.key));

	// Remove 0x20 padding
	boost::trim_right(name);
	boost::trim_right(key);

	// TODO: Move verify code into separate function
	if (m_serverVerifyNames) {
		// Local player checks--works for xxx.xxx.xxx.0/24 subnets
		std::string ipString = client->GetIpString();
		uint32_t localIp = sf::IpAddress::getLocalAddress().toInteger();
		uint32_t ip = sf::IpAddress(ipString).toInteger();
		uint32_t netmask = 0xffffff00;

		bool isLocalPlayer = (ipString == "127.0.0.1" || ((localIp & netmask) == (ip & netmask)));

		if (isLocalPlayer) {
			LOG(LogLevel::kInfo, "Bypassing name verification for local player %s", name.c_str());
		} else {
			std::string saltName = m_salt + name;
			unsigned char result[MD5_DIGEST_LENGTH];

			MD5((unsigned char*)saltName.c_str(), saltName.length(), result);

			char mdString[33];

			assert(MD5_DIGEST_LENGTH < sizeof(mdString));

			for (int i = 0; i < MD5_DIGEST_LENGTH; ++i)
				sprintf(&mdString[i*2], "%02x", (unsigned int)result[i]);

			if (strcmp(mdString, key.c_str()) != 0) {
				LOG(LogLevel::kDebug, "Refusing player %s (sent invalid key)", name.c_str());
				KickClient(client, "Invalid key");
				return;
			}
		}
	}

	// Pass authed player to events
	auto table = cauthp_to_luatable(clientAuth);
	m_pluginHandler.TriggerEvent(EventType::kOnAuth, client, table);

	// Don't use default if a plugin set flag
	if (m_pluginHandler.GetEventFlag("NoDefaultCall"))
		return;

	Client* checkClient = GetClientByName(name);
	if (checkClient != nullptr)
		KickClient(checkClient, "Logged in from somewhere else");

	// Won't refuse client if replacing ghost player (see checkClient above)
	if (m_numClients >= m_maxClients && checkClient == nullptr) {
		LOG(LogLevel::kDebug, "Refusing player %s (server is full)", name.c_str());
		KickClient(client, "Server is full");
		return;
	}

	client->SetName(name);
	client->authed = true;

	m_numClients++;

	std::string ipString = client->GetIpString();

	uint8_t userType = 0x00;
	if (IsOperator(name)) {
		userType = 0x64;
		BroadcastMessage("&e" + name + " joined the game");
		LOG(LogLevel::kInfo, "Operator %s (%s) authenticated", name.c_str(), ipString.c_str());
	} else {
		BroadcastMessage("&e" + name + " joined the game");
		LOG(LogLevel::kInfo, "Player %s (%s) authenticated", name.c_str(), ipString.c_str());
	}

	client->SetUserType(userType);

	World* world = GetWorld("default");

	client->SetWorldName("default");

	// Do this before AddClient()
	SendInfo(client, m_serverName, m_serverMotd, m_version, userType);

	world->AddClient(client);

	SendMessage(client, "https://github.com/vexyl/MCHawk");
	SendMessage(client, "&eTry /goto freebuild to get started.");
}

void Server::OnMessage(Client* client, struct cmsgp clientMsg)
{
	std::string message((char*)clientMsg.msg, 0, sizeof(clientMsg.msg));
	std::string name = client->GetName();

	// Remove 0x20 padding
	boost::trim_right(message);

	// Chat mute check--IsChatMuted() unmutes if timer expires
	if (client->IsChatMuted()) {
		LOG(LogLevel::kInfo, "[Muted (%s)] %s", name.c_str(), message.c_str());
		return;
	}

	// Is it a command or a chat message?
	if (message.at(0) == '/') {
		LOG(LogLevel::kInfo, "[Command (%s)] %s", name.c_str(), message.c_str());

		// Prevents command spam/ClassicalSharp sending multiple lines on a command
		client->SetChatMute(1000); // mutes for 1 second

		// Strip leading '/'
		std::string command = message.substr(1);

		// Show help menu if empty and space for page numbers (e.g., / 2 shows help menu p. 2)
		if (command.empty() || command[0] == ' ')
			command.insert(0, "help");

		m_commandHandler.Handle(client, command);
	} else {
		// Done before message is modified for colors
		LOG(LogLevel::kInfo, "[BROADCAST] %s: %s", name.c_str(), message.c_str());

		// Name colors for normal/op (temporary until plugins are implemented)
		if (client->GetUserType() > 0)
			message.insert(0, "&e" + client->GetChatName() + "&f: ");
		else
			message.insert(0, "&7" + client->GetChatName() + "&f: ");

		// ClassicalSharp color codes
		// Why does the first color code get sent twice?
		// and this could be made more efficient by keeping track of pos of last color
		size_t colorPos = 0;
		while ((colorPos = message.find('%', colorPos)) != std::string::npos) {
			if (colorPos == (message.size() - 1))
				break;

			char c = message.at(colorPos+1);
			if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
				message[colorPos] = '&';

			colorPos++;
		}

		BroadcastMessage(message);
	}
}

void Server::HandlePacket(Client* client, uint8_t opcode)
{
	ClientStream& stream = client->stream;

	if (!client->authed) {
		if (opcode == CAUTH) {
			struct cauthp clientAuth;
			if (clientAuth.Read(stream)) {
				OnAuth(client, clientAuth);
			}
		} else {
			LOG(LogLevel::kDebug, "Dropped unauthorized client (%s)", client->GetIpString().c_str());
			client->active = false;
		}

		// Nothing else to be done for this client
		return;
	}

	switch (opcode) {
	case CMSG:
	{
		struct cmsgp clientMsg;

		if (clientMsg.Read(stream)) {
			OnMessage(client, clientMsg);

			auto table = cmsgp_to_luatable(clientMsg);
			m_pluginHandler.TriggerEvent(EventType::kOnMessage, client, table);
		}

		break;
	}
	case CPOS:
	{
		struct cposp clientPos;

		if (clientPos.Read(stream)) {
			auto table = make_luatable();
			m_pluginHandler.TriggerEvent(EventType::kOnPosition, client, table);

			GetWorld(client->GetWorldName())->OnPosition(client, clientPos);
		}

		break;
	}
	case CBLOCK:
	{
		struct cblockp clientBlock;

		if (clientBlock.Read(stream)) {
			auto table = cblockp_to_luatable(clientBlock);
			m_pluginHandler.TriggerEvent(EventType::kOnBlock, client, table);

			GetWorld(client->GetWorldName())->OnBlock(client, clientBlock);
		}
		break;
	}
	default:
		LOG(LogLevel::kWarning, "Unknown opcode 0x%x from client (%s)", opcode, client->GetIpString().c_str());
		KickClient(client, "Unknown opcode received");
		break;
	}
}

void Server::Tick()
{
	// Send heartbeat to server list
	if (m_heartbeatClock.getElapsedTime().asSeconds() >= kHeartbeatTime) {
		SendHeartbeat();
		m_heartbeatClock.restart();
	}

	// World tick
	for (auto& obj : m_worlds)
		obj.second->Tick();

	// Accept new sockets
	sf::TcpSocket* socket = new sf::TcpSocket();

	socket->setBlocking(false);

	if (m_listener.accept(*socket) == sf::Socket::Done)
		OnConnect(socket);
	else
		delete socket;

	// Update clients
	auto it = m_clients.begin();
	while(it != m_clients.end()) {
		sf::Socket::Status status = (*it)->stream.poll();
		if (status == sf::Socket::Disconnected)
			(*it)->active = false;

		// If any data is in stream, pass HandlePacket the opcode (first byte of buffer)
		if ((*it)->stream.count > 0)
			HandlePacket(*it, (*it)->stream.buf[0]);

		(*it)->ProcessPacketsInQueue();

		// Deletes inactive clients and sends despawn packet to all active clients if necessary
		if (!(*it)->active) {
			bool authed = (*it)->authed;
			std::string name = (*it)->GetName();
			std::string ipString = (*it)->GetIpString();
			int8_t pid = (*it)->GetPid();
			World* world = GetWorld((*it)->GetWorldName());

			// Client removed here because DespawnClient() would send to this inactive client as well
			delete (*it)->stream.socket;
			it = m_clients.erase(it);

			if (authed) {
				LOG(LogLevel::kInfo, "Player %s disconnected (%s)", name.c_str(), ipString.c_str());
				BroadcastMessage("&ePlayer " + name + " left the game.");
				world->RemoveClient(pid);

				if (m_numClients > 0)
					m_numClients--;
			} else {
				LOG(LogLevel::kInfo, "Client disconnected (%s)", ipString.c_str());
			}
		} else {
			++it;
		}
	}
}

// Doesn't use https
void Server::SendHeartbeat()
{
	// Setting to disable heartbeat in config file
	if (!m_serverHeartbeat) return;

	char postData[256];
	std::string software = "MCHawk";
	std::string isPublic = (m_serverPublic ? "True" : "False");

	snprintf(postData, sizeof(postData), "public=%s&max=%d&users=%d&port=%d&version=%d&salt=%s&name=%s&software=%s", isPublic.c_str(), m_maxClients, m_numClients, m_port, m_version, m_salt.c_str(), m_serverName.c_str(), software.c_str());

	sf::Http http;
	http.setHost("http://www.classicube.net/");

	sf::Http::Request request;

	request.setMethod(sf::Http::Request::Post);
	request.setUri("/server/heartbeat");
	request.setBody(postData);

	sf::Http::Response response = http.sendRequest(request);

	if (response.getStatus() != sf::Http::Response::Status::Ok)
		LOG(LogLevel::kWarning, "Failed to send heartbeat");
}

///////////////////////////////////
/* BEGIN CLIENT HELPER FUNCTIONS */
///////////////////////////////////

void Server::KickClient(Client* client, std::string reason)
{
	if (reason.empty())
		reason = "Kicked";

	SendKick(client, reason);

	// FIXME: Maybe queue disconnect? Sometimes client gets I/O error when kicked
	client->active = false;

	if (client->authed) {
		LOG(LogLevel::kInfo, "Kicked player %s (%s | %s)", client->GetName().c_str(), client->GetIpString().c_str(), (reason.empty() ? "None" : reason.c_str()));
	} else {
		LOG(LogLevel::kInfo, "Kicked unauthorized player (%s | %s)", client->GetIpString().c_str(), (reason.empty() ? "None" : reason.c_str()));
	}
}

// This does not handle color codes on split lines nor breaking after words
void Server::SendWrappedMessage(Client* client, std::string message)
{
	int max = 64;
	int pos = 0;

	int length = message.length();
	while (pos < length) {
		int diff = length - pos;
		int count = std::min(diff, max);

		std::string partialMessage = message.substr(pos, count);

		SendMessage(client, partialMessage);

		pos += count;
	}
}

void Server::SendSystemMessage(Client* client, std::string message)
{
	SendWrappedMessage(client, "&e[SYSTEM]: " + message);
}

void Server::SendSystemWideMessage(std::string message)
{
	for (auto& client : m_clients)
		SendSystemMessage(client, message);
}

void Server::BroadcastMessage(std::string message)
{
	for (auto& client : m_clients)
		SendWrappedMessage(client, message);
}

Client* Server::GetClientByName(std::string name, bool exact)
{
	Client* client = nullptr;

	boost::algorithm::to_lower(name);
	for (auto& obj : m_clients) {
		std::string clientName = obj->GetName();

		boost::algorithm::to_lower(clientName);

		// Exact match
		if (name == clientName)
			return obj;

		// Return closest result unless there are multiple possible names
		if (!exact && clientName.find(name) == 0) {
			if (client != nullptr) {
				client = nullptr;
				break;
			}

			client = obj;
		}
	}

	return client;
}

std::vector<ClientInfo> Server::GetAllClientInfo() const
{
	std::vector<ClientInfo> infoList;

	for (auto& obj : m_clients) {
		ClientInfo info;
		info.name = obj->GetName();
		info.worldName = obj->GetWorldName();
		info.ip = obj->GetIpString();

		infoList.push_back(info);
	}

	return infoList;
}

bool Server::IsOperator(std::string name)
{
	std::ifstream operatorFile("ops.txt");
	std::string operatorName;

	if (!operatorFile.is_open())
		LOG(LogLevel::kDebug, "Couldn't open ops file");

	while (std::getline(operatorFile, operatorName)) {
		if (name == operatorName)
			return true;
	}

	return false;
}

/////////////////////////////////
/* END CLIENT HELPER FUNCTIONS */
/////////////////////////////////

// World stuff

void Server::AddWorld(World* world)
{
	std::string name = world->GetName();
	auto i = m_worlds.find(name);

	if (i != m_worlds.end()) {
		LOG(LogLevel::kDebug, "World '%s' already exists", name.c_str());
		return;
	}

	m_worlds.insert(std::make_pair(name, world));

	LOG(LogLevel::kDebug, "Added world '%s'", name.c_str());
}

void Server::RemoveWorld(std::string name)
{
	auto i = m_worlds.find(name);

	if (i != m_worlds.end()) {
		LOG(LogLevel::kDebug, "World '%s' does not exist", name.c_str());
		return;
	}

	m_worlds.erase(i);

	LOG(LogLevel::kDebug, "Removed world '%s'", name.c_str());
}

World* Server::GetWorld(std::string name)
{
	auto i = m_worlds.find(name);

	if (i == m_worlds.end()) {
		LOG(LogLevel::kDebug, "World '%s' does not exist", name.c_str());
		return nullptr;
	}

	return i->second;
}

std::vector<std::string> Server::GetWorldNames()
{
	std::vector<std::string> worldNames;

	for (auto& obj : m_worlds)
		worldNames.push_back(obj.first);

	return worldNames;
}
