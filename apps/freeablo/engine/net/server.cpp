#include "server.h"

#include <iostream>
#include <thread>

#include "../../faworld/position.h"
#include "../../faworld/player.h"
#include "../../faworld/monster.h"

#include "netcommon.h"
#include "netops.h"

namespace Engine
{
    Server::Server(const FAWorld::PlayerFactory& factory) : mPlayerFactory(factory)
    {
        mAddress.port = 6666;
        mAddress.host = ENET_HOST_ANY;
        mHost = enet_host_create(&mAddress, 32, 2, 0, 0);
    }

    Server::~Server()
    {
        for (size_t i = 0; i < mClients.size(); i++)
            enet_peer_disconnect(mClients[i], 0);
        
        enet_host_destroy(mHost);
    }

    void Server::update(uint32_t tick)
    {
        if (tick % 2 == 0) // server only send packets every second tick
            return;

        #ifdef ENABLE_NET_STALL_ON_TIMEOUT
        {
            uint32_t stallThresh = getStallThreshold();
                           
            for (size_t i = 0; i < mClients.size(); i++)
            {
                uint32_t diff = tick - mServersClientData[mClients[i]->connectID].lastReceiveTick;
                if (diff > stallThresh)
                    std::cout << "STALLED BY NETWORK" << std::endl;

                while (diff > stallThresh)
                {
                    updateImp(tick);
                    diff = tick - mServersClientData[mClients[i]->connectID].lastReceiveTick;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        }
        #endif

        updateImp(tick);
    }

    void Server::updateImp(uint32_t tick)
    {
        ENetEvent event;
        while(enet_host_service(mHost, &event, 0))
        {
            switch(event.type)
            {
                case ENET_EVENT_TYPE_CONNECT:
                {
                    handleNewClient(event.peer);
                    break;
                }

                case ENET_EVENT_TYPE_RECEIVE:
                {
                    readClientPacket(event, tick);
                    break;
                }

                case ENET_EVENT_TYPE_DISCONNECT:
                {
                    std::cout << "DISCONNECT" << std::endl;
                    break;
                }

                case ENET_EVENT_TYPE_NONE:
                {
                    break;
                }

                default:
                {
                    std::cout << "UNHANDLED" << std::endl;
                    break;
                }
            }
        }

        if(mClients.size() > 0)
            sendServerPacket(tick);

        enet_host_flush(mHost);
    }

    FAWorld::Player* Server::spawnPlayer(int32_t id)
    {
        auto newPlayer = mPlayerFactory.create("Warrior");
        newPlayer->mPos = FAWorld::Position(76, 68);
        newPlayer->destination() = newPlayer->mPos.current();

        if (id != -1)
            newPlayer->mId = id;

        return newPlayer;
    }

    void Server::sendLevel(size_t levelIndex, ENetPeer *peer)
    {
        FAWorld::GameLevel* level = FAWorld::World::get()->getLevel(levelIndex);
        std::string data = level->serialiseToString();

        WritePacket packet = getWritePacket(PacketType::Level, data.size(), true, WritePacketResizableType::Resizable);
        
        uint32_t dataSize = data.size();
        packet.writer.handleInt32(dataSize);
        packet.writer.handleString((uint8_t*)&data[0], data.length());
        sendPacket(packet, peer);
    }

    void Server::sendServerPacket(uint32_t tick)
    {
        FAWorld::World& world = *FAWorld::World::get();

        auto packet = getWritePacket(Engine::PacketType::GameUpdateMessage, 512, false);

        // write header
        int64_t headerPos = packet.writer.tell();
        ServerPacketHeader header;
        header.numPlayers = 0;
        header.tick = tick;

        header.faSerial(packet.writer);

        std::vector<FAWorld::Actor*> allActors;
        world.getAllActors(allActors);

        std::sort(allActors.begin(), allActors.end(), [](FAWorld::Actor* a, FAWorld::Actor* b) -> bool
        {
            return a->getPriority() > b->getPriority();
        });

        Serial::Error::Error err = Serial::Error::Success;

        bool packetFull = false;
        for (auto actor : allActors)
        {
            if (!packetFull)
            {
                int32_t classId = actor->getClassId();
                int32_t actorId = actor->getId();

                int64_t startPos = packet.writer.tell();

                if (err == Serial::Error::Success)
                    err = packet.writer.handleInt<0, 1024>(classId);
                if (err == Serial::Error::Success)
                    err = packet.writer.handleInt32(actorId);
                if (err == Serial::Error::Success)
                    err = actor->streamHandle(packet.writer);

                if (err == Serial::Error::Success)
                {
                    header.numPlayers++;
                }
                else if (err == Serial::Error::EndOfStream)
                {
                    packet.writer.seek(startPos, Serial::BSPos::Start);
                    packetFull = true;
                }
                else
                {
                    std::cerr << "Serialisation send error " << Serial::Error::getName(err) << std::endl;
                    exit(1);
                }
            }

            actor->tickDone(!packetFull);
        }

        packet.writer.fillWithZeros();

        // rewrite packet header with correct object count
        packet.writer.seek(headerPos, Serial::BSPos::Start);
        header.faSerial(packet.writer);

        broadcastPacket(packet, mHost);
    }


    void Server::readClientPacket(ENetEvent& event, uint32_t tick)
    {
        std::shared_ptr<ReadPacket> packet = getReadPacket(event.packet);

        switch (packet->type)
        {
            case PacketType::Sprite:
            {
                readSpriteRequest(packet, event.peer);
                break;
            }

            case PacketType::ClientToServerUpdate:
            {
                readClientUpdatePacket(packet, event.peer, tick);
                break;
            }

            default:
            {
                assert(false && "BAD PACKET TYPE RECEIVED");
            }
        }
    }

    Serial::Error::Error Server::readClientUpdatePacket(std::shared_ptr<ReadPacket> packet, ENetPeer* peer, uint32_t tick)
    {
        ClientPacket data;
        serialise_object(packet->reader, data);

        auto world = FAWorld::World::get();

        mServersClientData[peer->connectID].lastReceiveTick = tick;

        //std::cout << "GOT MESSAGE " << mTick << " " << mServersClientData[event.peer->connectID].lastReceiveTick << std::endl;

        auto player = mServersClientData[peer->connectID].player;

        player->destination().first = data.destX;
        player->destination().second = data.destY;

        if (data.levelIndex != -1 && (player->getLevel() == NULL || data.levelIndex != (int32_t)player->getLevel()->getLevelIndex()))
        {
            auto level = world->getLevel(data.levelIndex);

            if (player->getLevel() != NULL && data.levelIndex < (int32_t)player->getLevel()->getLevelIndex())
                player->mPos = FAWorld::Position(level->downStairsPos().first, level->downStairsPos().second);
            else
                player->mPos = FAWorld::Position(level->upStairsPos().first, level->upStairsPos().second);

            player->setLevel(level);
        }

        return Serial::Error::Success;
    }

    Serial::Error::Error Server::readSpriteRequest(std::shared_ptr<ReadPacket> packet, ENetPeer* peer)
    {
        uint32_t numEntries = 0;
        serialise_int32(packet->reader, numEntries);

        std::vector<uint32_t> requestedSprites;
        std::vector<std::string> paths;

        auto renderer = FARender::Renderer::get();

        for (uint32_t i = 0; i < numEntries; i++)
        {
            uint32_t index = 0;
            serialise_int32(packet->reader, index);

            requestedSprites.push_back(index);
            paths.push_back(renderer->getPathForIndex(index));
        }

        Serial::Error::Error err = answerSpriteRequest(paths, requestedSprites, peer);
        if (err != Serial::Error::Success)
            return err;

        return Serial::Error::Success;
    }

    Serial::Error::Error Server::answerSpriteRequest(std::vector<std::string>& paths, std::vector<uint32_t>& requestedSprites, ENetPeer* peer)
    {
        WritePacket packet = getWritePacket(PacketType::Sprite, 0, true, WritePacketResizableType::Resizable);

        uint32_t numEntries = paths.size();
        serialise_int32(packet.writer, numEntries);

        for (uint32_t i = 0; i < paths.size(); i++)
        {
            uint32_t len = paths[i].length();

            serialise_int32(packet.writer, requestedSprites[i]);
            serialise_int32(packet.writer, len);
            serialise_str(packet.writer, (uint8_t*)&paths[i][0], len);

            std::cout << "SPRITE RESPONSE: " << requestedSprites[i] << " " << paths[i] << std::endl;
        }

        packet.writer.fillWithZeros();

        sendPacket(packet, peer);

        return Serial::Error::Success;
    }

    void Server::handleNewClient(ENetPeer* peer)
    {
        #ifdef ENABLE_NET_STALL_ON_TIMEOUT
            enet_peer_timeout(peer, std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max());
        #endif

        auto newPlayer = spawnPlayer(-1);

        auto packet = getWritePacket(Engine::PacketType::NewClient, 0, true, Engine::WritePacketResizableType::Resizable);
        int32_t id = newPlayer->getId();
        packet.writer.handleInt32(id);
        Engine::sendPacket(packet, peer);

        mClients.push_back(peer);
        mServersClientData[peer->connectID] = ClientData(newPlayer);

        // TODO: send levels on-demand
        sendLevel(0, peer);
        sendLevel(1, peer);
    }
}
