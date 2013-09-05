//
//  main.cpp
//  Domain Server 
//
//  Created by Philip Rosedale on 11/20/12.
//  Copyright (c) 2012 High Fidelity, Inc. All rights reserved.
//
//  The Domain Server keeps a list of nodes that have connected to it, and echoes that list of
//  nodes out to nodes when they check in.
//
//  The connection is stateless... the domain server will set you inactive if it does not hear from
//  you in LOGOFF_CHECK_INTERVAL milliseconds, meaning your info will not be sent to other users.
//
//  Each packet from an node has as first character the type of server:
//
//  I - Interactive Node
//  M - Audio Mixer
//

#include <arpa/inet.h>
#include <fcntl.h>
#include <map>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#import "mongoose.h"

#include "Assignment.h"
#include "NodeList.h"
#include "NodeTypes.h"
#include "Logstash.h"
#include "PacketHeaders.h"
#include "SharedUtil.h"

const int DOMAIN_LISTEN_PORT = 39999;
unsigned char packetData[MAX_PACKET_SIZE];

const int NODE_COUNT_STAT_INTERVAL_MSECS = 5000;

unsigned char* addNodeToBroadcastPacket(unsigned char* currentPosition, Node* nodeToAdd) {
    *currentPosition++ = nodeToAdd->getType();
    
    currentPosition += packNodeId(currentPosition, nodeToAdd->getNodeID());
    currentPosition += packSocket(currentPosition, nodeToAdd->getPublicSocket());
    currentPosition += packSocket(currentPosition, nodeToAdd->getLocalSocket());
    
    // return the new unsigned char * for broadcast packet
    return currentPosition;
}

// This function will be called by mongoose on every new request.
static int begin_request_handler(struct mg_connection *conn) {
    NodeList* nodeList = NodeList::getInstance();
    
    char agentDescriptions[NodeList::getInstance()->getNumAliveNodes() * 100];
    
    int content_length = 0;
    
    for (NodeList::iterator node = nodeList->begin(); node != nodeList->end(); node++) {
        content_length += snprintf(agentDescriptions + content_length,
                                   sizeof(agentDescriptions) - content_length,
                                   "%s on %s:%d\n",
                                   node->getTypeName(),
                                   inet_ntoa(((sockaddr_in*) node->getActiveSocket())->sin_addr),
                                   ntohs(((sockaddr_in*) node->getActiveSocket())->sin_port));
    }
    
    // Send HTTP reply to the client
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: %d\r\n"        // Always set Content-Length
              "\r\n"
              "%s",
              content_length, agentDescriptions);
    
    // Returning non-zero tells mongoose that our function has replied to
    // the client, and mongoose should not send client any more data.
    return 1;
}

int main(int argc, char* const argv[]) {
    NodeList* nodeList = NodeList::createInstance(NODE_TYPE_DOMAIN, DOMAIN_LISTEN_PORT);
	// If user asks to run in "local" mode then we do NOT replace the IP
	// with the EC2 IP. Otherwise, we will replace the IP like we used to
	// this allows developers to run a local domain without recompiling the
	// domain server
	bool isLocalMode = cmdOptionExists(argc, (const char**) argv, "--local");
	if (isLocalMode) {
		printf("NOTE: Running in local mode!\n");
	} else {
		printf("--------------------------------------------------\n");
		printf("NOTE: Not running in local mode. \n");
		printf("If you're a developer testing a local system, you\n");
		printf("probably want to include --local on command line.\n");
		printf("--------------------------------------------------\n");
	}

    setvbuf(stdout, NULL, _IOLBF, 0);
    
    ssize_t receivedBytes = 0;
    char nodeType = '\0';
    
    unsigned char broadcastPacket[MAX_PACKET_SIZE];
    int numHeaderBytes = populateTypeAndVersion(broadcastPacket, PACKET_TYPE_DOMAIN);
    
    unsigned char* currentBufferPos;
    unsigned char* startPointer;
    
    sockaddr_in nodePublicAddress, nodeLocalAddress;
    nodeLocalAddress.sin_family = AF_INET;
    
    in_addr_t serverLocalAddress = getLocalAddress();
    
    nodeList->startSilentNodeRemovalThread();
    
    timeval lastStatSendTime = {};
    
    // loop the parameters to see if we were passed a pool for assignment
    int parameter = -1;
    const char ALLOWED_PARAMETERS[] = "p::-local::";
    const char POOL_PARAMETER_CHAR = 'p';
    
    char* assignmentPool = NULL;
    
    while ((parameter = getopt(argc, argv, ALLOWED_PARAMETERS)) != -1) {
        if (parameter == POOL_PARAMETER_CHAR) {
            // copy the passed assignment pool
            int poolLength = strlen(optarg);
            assignmentPool = new char[poolLength + sizeof(char)];
            strcpy(assignmentPool, optarg);
        }
    }
    
    // start a mongoose server to publish information about the domain-server
    struct mg_context *ctx;
    struct mg_callbacks callbacks;
    
    // List of options. Last element must be NULL.
    const char *options[] = {"listening_ports", "8080", NULL};
    
    // Prepare callbacks structure. We have only one callback, the rest are NULL.
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.begin_request = begin_request_handler;
    
    // Start the web server.
    ctx = mg_start(&callbacks, NULL, options);
    
    while (true) {
        
        if (!nodeList->soloNodeOfType(NODE_TYPE_AUDIO_MIXER)) {
            // create an assignment to send, ask for an audio mixer, pass the assignment pool if it exists
            Assignment mixerAssignment(Assignment::Create, Assignment::AudioMixer, assignmentPool);
            nodeList->sendAssignment(mixerAssignment);
        } else if (!nodeList->soloNodeOfType(NODE_TYPE_AVATAR_MIXER)) {
            // create an assignment to send, ask for an avatar mixer, pass the assignment pool if it exists
            Assignment avatarAssignment(Assignment::Create, Assignment::AvatarMixer, assignmentPool);
            nodeList->sendAssignment(avatarAssignment);
        }
        
        
        if (nodeList->getNodeSocket()->receive((sockaddr *)&nodePublicAddress, packetData, &receivedBytes) &&
            (packetData[0] == PACKET_TYPE_DOMAIN_REPORT_FOR_DUTY || packetData[0] == PACKET_TYPE_DOMAIN_LIST_REQUEST) &&
            packetVersionMatch(packetData)) {
            // this is an RFD or domain list request packet, and there is a version match
            std::map<char, Node *> newestSoloNodes;
            
            int numBytesSenderHeader = numBytesForPacketHeader(packetData);
            
            nodeType = *(packetData + numBytesSenderHeader);
            int numBytesSocket = unpackSocket(packetData + numBytesSenderHeader + sizeof(NODE_TYPE),
                                              (sockaddr*) &nodeLocalAddress);
            
            sockaddr* destinationSocket = (sockaddr*) &nodePublicAddress;
            
            // check the node public address
            // if it matches our local address we're on the same box
            // so hardcode the EC2 public address for now
            if (nodePublicAddress.sin_addr.s_addr == serverLocalAddress) {
            	// If we're not running "local" then we do replace the IP
            	// with 0. This designates to clients that the server is reachable
                // at the same IP address 
            	if (!isLocalMode) {
	                nodePublicAddress.sin_addr.s_addr = 0;
                    destinationSocket = (sockaddr*) &nodeLocalAddress;
	            }
            }
            
            Node* newNode = nodeList->addOrUpdateNode((sockaddr*) &nodePublicAddress,
                                                      (sockaddr*) &nodeLocalAddress,
                                                      nodeType,
                                                      nodeList->getLastNodeID());
            
            if (newNode->getNodeID() == nodeList->getLastNodeID()) {
                nodeList->increaseNodeID();
            }
            
            currentBufferPos = broadcastPacket + numHeaderBytes;
            startPointer = currentBufferPos;
            
            unsigned char* nodeTypesOfInterest = packetData + numBytesSenderHeader + sizeof(NODE_TYPE)
                + numBytesSocket + sizeof(unsigned char);
            int numInterestTypes = *(nodeTypesOfInterest - 1);
            
            if (numInterestTypes > 0) {
                // if the node has sent no types of interest, assume they want nothing but their own ID back
                for (NodeList::iterator node = nodeList->begin(); node != nodeList->end(); node++) {
                    if (!node->matches((sockaddr*) &nodePublicAddress, (sockaddr*) &nodeLocalAddress, nodeType) &&
                            memchr(nodeTypesOfInterest, node->getType(), numInterestTypes)) {
                        // this is not the node themselves
                        // and this is an node of a type in the passed node types of interest
                        // or the node did not pass us any specific types they are interested in

                        if (memchr(SOLO_NODE_TYPES, node->getType(), sizeof(SOLO_NODE_TYPES)) == NULL) {
                            // this is an node of which there can be multiple, just add them to the packet
                            // don't send avatar nodes to other avatars, that will come from avatar mixer
                            if (nodeType != NODE_TYPE_AGENT || node->getType() != NODE_TYPE_AGENT) {
                                currentBufferPos = addNodeToBroadcastPacket(currentBufferPos, &(*node));
                            }
                        
                        } else {
                            // solo node, we need to only send newest
                            if (newestSoloNodes[node->getType()] == NULL ||
                                newestSoloNodes[node->getType()]->getWakeMicrostamp() < node->getWakeMicrostamp()) {
                                // we have to set the newer solo node to add it to the broadcast later
                                newestSoloNodes[node->getType()] = &(*node);
                            }
                        }
                    }
                }
                
                for (std::map<char, Node *>::iterator soloNode = newestSoloNodes.begin();
                     soloNode != newestSoloNodes.end();
                     soloNode++) {
                    // this is the newest alive solo node, add them to the packet
                    currentBufferPos = addNodeToBroadcastPacket(currentBufferPos, soloNode->second);
                }
            }
                        
            // update last receive to now
            uint64_t timeNow = usecTimestampNow();
            newNode->setLastHeardMicrostamp(timeNow);
            
            if (packetData[0] == PACKET_TYPE_DOMAIN_REPORT_FOR_DUTY
                && memchr(SOLO_NODE_TYPES, nodeType, sizeof(SOLO_NODE_TYPES))) {
                newNode->setWakeMicrostamp(timeNow);
            }
            
            // add the node ID to the end of the pointer
            currentBufferPos += packNodeId(currentBufferPos, newNode->getNodeID());
            
            // send the constructed list back to this node
            nodeList->getNodeSocket()->send(destinationSocket,
                                            broadcastPacket,
                                            (currentBufferPos - startPointer) + numHeaderBytes);
        }
        
        if (Logstash::shouldSendStats()) {
            if (usecTimestampNow() - usecTimestamp(&lastStatSendTime) >= (NODE_COUNT_STAT_INTERVAL_MSECS * 1000)) {
                // time to send our count of nodes and servers to logstash
                const char NODE_COUNT_LOGSTASH_KEY[] = "ds-node-count";
                
                Logstash::stashValue(STAT_TYPE_TIMER, NODE_COUNT_LOGSTASH_KEY, nodeList->getNumAliveNodes());
                
                gettimeofday(&lastStatSendTime, NULL);
            }
        }
    }
    
    // Stop the server.
    mg_stop(ctx);

    return 0;
}

