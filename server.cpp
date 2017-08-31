
#include <cstdlib>
#include <iostream>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <fstream>
#include <map>
#include <list>
#include <time.h>
#include <pthread.h>
#include <stdint.h>



using namespace std;
/*
Displays menu on terminal 

*/

#define size_recv_buff 1024
#define size_resp_buff 1024

//Different msg type so as to distinguish packets 
#define msg_type_publish 0
#define msg_type_search 1
#define msg_type_fetch 2
#define msg_type_ping 3
#define msg_type_err 10

//Interval of time at which server expects a ping message from client to make sure that client is still connected
//Else it will disconnect the client
#define ping_interval 20

/*Class to represent a client - it stores some info about client too*/

class ClientHandler {
public:

    char recv_buff[size_recv_buff]; // packet for a client is read in this buffer from main thread
    int recv_len; //len of packet received is stored in this
    int index_read; // To keep track of index till which data has been read into buffer - as data may be received in chunks so need to store complete
    // packet in same buffer ... index_read is used for that 

    list< string > filenames_list; //It stores the list of files that client has published 
    //It is used to find out all files client has published so that entry for these files can be deleted  
    //from map fileToClientList when client connection is to be closed 

    int sockfd; //It stores the socket file descriptor for this particular client 

    long last_ping_time; // It stores last time at which a ping message was received from client

    ClientHandler() {
        memset(recv_buff, 0, size_recv_buff);
        recv_len = 0;
        index_read = 0;
        sockfd = -1;
        last_ping_time = 0;
    }

    int dataHandler(char* resp_buff, int* resp_len);
    int handlePublish();
    int handleSearch(char* resp_buff, int* resp_len);
    int handlePing();
    void addFilenameToList(string filename);
    int removeEntriesFromfileToClientList();
};

//It handles all transport related functionalities like send,recv etc
//It gives a separate module to handle transport - here TCP ( transport can be changed by just changing functionalities within this module)

class Transport {
private:
    int sockfd; //Socket file descriptor for this transport created

public:

    Transport() {

    }
    int createAndBindSocket(const char* ip, uint16_t port);
    int sendData(int sockfd, char* sendbuff, int len);
    int recvData(int sockfd, char *recvbuff, int max_len);
    int acceptConn(struct sockaddr_in* client_addr, int* len);
};

// structure to store listen ip,port of client at which it will server the published file

struct client_listen_info {
    uint32_t listen_ip;
    uint16_t listen_port;
    int sockfd;
};

//Using 2 maps

/* it stores (sockfd,CLientHandler obj)  mapping 
 It is used to find out ClientHandlerObj for a particular sockfd whenever needed  
 eg - if a packet is received on some sockfd
    - ping delayed from a client so this can help find clientHandlerObj whose ping is delayed 
        and take corresponding action
 */
map<int, ClientHandler> sockToClient;

/* It stores (filename,list of info ) mapping 
 * list of file info has - listen ip,port at which this file will be served by various clients .It also stores sockfd for the connection with client
 *                          Its a list as same file can be published by many clients
 *   */
map< string, list< struct client_listen_info> > fileToClientList;


/****************************************************************************************************************************************************
                                                    TRANSPORT
 ****************************************************************************************************************************************************/

/*It creates a socket
        binds a socket to ip,port provided
        starts listening at that socket
 * 
 * If success returns sockfd created
 * else returns < 0
 */
int Transport::createAndBindSocket(const char* ip, uint16_t port) {
    int ret = -1;
    struct sockaddr_in serv_addr;

    //make structure for server addr
    memset(&serv_addr, 0, sizeof (serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = inet_addr(ip);
    //    serv_addr.sin_addr.s_addr = INADDR_ANY;

    //create listening socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        //error 
        close(sockfd);
        return -1;
    }

    //bind socket to port 
    ret = bind(sockfd, (struct sockaddr *) &serv_addr, sizeof (struct sockaddr));
    if (ret < 0) {
        //bind failed
        perror("bind failed");
        close(sockfd);
        return -2;
    }

    // set server to listening mode
    ret = listen(sockfd, 50);
    if (ret < 0) {
        return -3;
    }

    return sockfd;
}

/* It is used to send data on transport 
 * if success returns no of bytes sent 
 * if failure returns <0 
 */

int Transport::sendData(int sockfd, char* sendbuff, int len) {

    return send(sockfd, sendbuff, len, 0);

}

/* It is used to receive data on transport 
 * if success returns no of bytes received 
 * if failure returns <0 
 */
int Transport::recvData(int sockfd, char *recvbuff, int max_len) {

    return recv(sockfd, recvbuff, max_len, 0);

}

/*It accepts connection 
 * If success sets client addr from which connection was initiated and returns new socket file descriptor for client
 * Else returns < 0
 */

int Transport::acceptConn(struct sockaddr_in* client_addr, int* len) {

    int ret = -1;

    ret = accept(sockfd, (struct sockaddr *) client_addr, (socklen_t *) len);
    if (ret < 0) {
        perror("accept");
        return -1;
    }
    return ret;
}

/************************************************************CLIENT_HANDLER**************************************************************************************/

/*
 * It handles packet received from client
 * Based on type of message performs suitable action
 * 
 * 1st byte - msg type
 * 2nd 3rd byte - msg len               
 *              
 * @param resp_buff     //If data is to be sent back it is filled in this buffer
 * @param resp_len      //If data is to be sent back ,length of data is set in this
 * @return 0 if data is to be sent back as response
 *         -1 If no resp is to be sent back
 */
int ClientHandler::dataHandler(char* resp_buff, int* resp_len) {

    int msg_type = -1;
    int ret = -1;

    // to print packet in hex
    /*for (i = 0; i < recv_len; i++) {
        printf("%.02x ", recv_buff[i]);
    }*/

    msg_type = *(uint8_t*) recv_buff;

    //cout << "msg type  " << msg_type << endl;
    switch (msg_type) {
        case msg_type_publish:
            // publish msg

            cout << " publish msg " << endl;

            //1st byte read - msg type and next 2 bytes are length of msg
            handlePublish();

            // nothing to be sent back
            return -1;
            break;
        case msg_type_search:
            // search msg
            cout << " search msg " << endl;

            //1st byte read - msg type and next 2 bytes are length of msg
            // need to send back some data prepare packet and fill in resp_buff
            if ((ret = handleSearch(resp_buff, resp_len)) < 0) {

                cout << " file not found " << endl;
            }
            // to show need to send response back return 0
            return 0;
            break;
        case msg_type_ping:
            // ping msg
            //cout << " ping msg " << endl;
            //update time
            handlePing();
            //cout << last_ping_time << endl;
            // nothing to be sent back so return -1
            return -1;
            break;
        default:
            //wrong packet type
            cout << "Wrong packet type " << endl;
            // nothing to be sent back so return -1
            return -1;
            break;
    }

}


/* If the message type in the request packet sent by client was publish
 * Fetch listen ip,port at which client will serve this file
 * Fetch filename
 * Store (filename , list) in fileToClientList map 
 * If there is an existing entry for this filename ie some other client has published this file then add this listen ip,port to existing list
 * corresponding to filename (if ip,port is not in that list)
 * 
 * Make an entry for this filename in filenames_list list -to reflect that client has published this file
 * 
 */

/*  
 ***************************** showing byte wise publish packet content ************************
                        0   1    2   3   4  5   6   7   8   9   10   11   12 ....... 12+i 
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                        |msg type | packet|   Listen ip  |Listen  |Length of| filename  |
                        |         | length|   (4 bytes)  |port    | filename|(i bytes)  |
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ***********************************************************************************************/

int ClientHandler::handlePublish() {


    uint32_t client_publish_ip = 0; //Listen ip at which client will serve this file
    uint16_t client_publish_port = 0; // listen port at which client will serve this file

    int index_recv_buff = 3; // skip msg type , len of msg( 2 bytes)

    //struct in_addr ip_addr;
    map<string, list<struct client_listen_info> >::iterator it; //To iterate over map fileToClientList
    list<struct client_listen_info > clientInfoList; // Create a list for a filename if no entry in map for this filename
    list<struct client_listen_info >* ptr_clientInfoList; // If entry exists for filename in map
    // Get pointer to corresponding list and add this new ip,port in it

    list<struct client_listen_info >::iterator it_list; //Iterator to iterate over list
    struct client_listen_info clientInfo; //Structure to store listen ip,port this will be inserted in list
    int entryExists = 0; //To check if entry for filename exists or not in map 0 shows it doesn't exist and 1 shows it exists

    //read ip from buffer
    client_publish_ip = ntohl(*(uint32_t*) (recv_buff + index_recv_buff));

    //ip_addr.s_addr = client_publish_ip;
    //cout << " ip in dotted decimal " << inet_ntoa(ip_addr) << endl;
    index_recv_buff += 4;

    //read port from buffer
    client_publish_port = ntohs(*(uint16_t *) (recv_buff + index_recv_buff));
    //cout << "port " << client_publish_port << endl;
    index_recv_buff += 2;

    int len_of_filename = 0;
    string fileName;

    // read all filenames and check entry in map - currently just 1 filename but it is scalable and can be used if multiple files are published together
    while (index_recv_buff < recv_len) {

        // if lesser than 3 bytes to read - exit 
        // as atleast 2 bytes for len of filename is required
        //and atleast 1 byte for filename
        if ((recv_len - index_recv_buff) < 3) {
            cout << "Not sufficient bytes to read in publish packet" << endl;
            return -1;
        }

        fileName = "";
        len_of_filename = 0;
        //cout << " index recv buff just before reading len of file " << index_recv_buff << endl;
        len_of_filename = ntohs(*(uint16_t*) (recv_buff + index_recv_buff));
        index_recv_buff += 2;

        fileName.assign((char*) recv_buff + index_recv_buff, len_of_filename);
        index_recv_buff += len_of_filename;

        memset(&clientInfo, 0, sizeof (struct client_listen_info));
        clientInfo.listen_ip = client_publish_ip;
        clientInfo.listen_port = client_publish_port;
        clientInfo.sockfd = sockfd;

        // find entry for this filename in map
        it = fileToClientList.find(fileName);

        if (it == fileToClientList.end()) {
            //this file is not published by any yet
            //add a new entry
            clientInfoList.clear();
            clientInfoList.push_front(clientInfo);

            pair < map<string, list < struct client_listen_info> >::iterator, bool> ret;
            ret = fileToClientList.insert(pair<string, list<struct client_listen_info > >(fileName, clientInfoList));
            if (ret.second == true) {
                cout << " Inserted entry in map for " << fileName << endl;
            } else {
                cout << "Couldn't insert in map" << endl;
            }
             addFilenameToList(fileName);
        } else {
            //entry exists - fetch list and add to it this entry
            //cout << " entry for file exists " << it->first << endl;

            // check if there is no entry for same ip port ie structure then only add it to linked list else don't add to list
            ptr_clientInfoList = &(it->second); //It will point to list for this filename
            //Iterating over the list whose pointer we have just got 
            for (it_list = clientInfoList.begin(); it_list != clientInfoList.end(); it_list++) {

                if (it_list->listen_ip == clientInfo.listen_ip && it_list->listen_port == clientInfo.listen_port) {
                    entryExists = 1;
                    break;
                }
            }
            if (!entryExists) {
                ptr_clientInfoList->push_front(clientInfo);
                //add filename to local list so that it can be used later to remove all references from map 
                // when client is gone
                addFilenameToList(fileName);
            } else {
                cout << "Entry for this file exist with same ip port so don't add again " << endl;
            }
        }

        //cout << " index_recv_buff " << index_recv_buff << endl;
    }

    return 0;
}

/* adds filename to list filenames_list
 */
void ClientHandler::addFilenameToList(string filename) {

    filenames_list.push_front(filename);
    cout << " size of list " << filenames_list.size() << endl;
}

/* On client connection closure or if no ping received from client
 * Then need to delete entries from map fileToClientList 
 * Do it for all files published by this client ie all files in filenames_list
 * Check in list of map by sockfd , if sockfd matches delete that node in place
 */

int ClientHandler::removeEntriesFromfileToClientList() {
    
    //index into fileToClientList for each filename of this client 
    // In each list check if sockfd matches delete that node
    
    list<string>::iterator it;
    map<string, list<struct client_listen_info> >::iterator map_it;
    list<struct client_listen_info>* ptr_list_in_map;
    list<struct client_listen_info>::iterator list_in_map_it;
    int count = 0;

    //CHECK _IF THIS IS REQUIRED
    if (filenames_list.size() > 0) {
        for (it = filenames_list.begin(); it != filenames_list.end(); ++it) {

            map_it = fileToClientList.find(*it);
            
            if (map_it == fileToClientList.end()) {
                // no list for this file so none to delete
                cout << "No list for this file name so none to delete " << *it << endl;
            } else {
               cout << "Enrty exists " << endl;
                ptr_list_in_map = &(map_it->second);        //Get the list from map
                if (ptr_list_in_map->size() <= 0) {
                    cout << " emptylist for this file name so none to delete " << *it << endl;
                } else {
                    //iterate on this list and delete entries for this client
                    
                    for (list_in_map_it = (*ptr_list_in_map).begin(); list_in_map_it != (*ptr_list_in_map).end();) {
                        if (list_in_map_it->sockfd == sockfd) {
                            list_in_map_it = ptr_list_in_map->erase(list_in_map_it);
                            count++;
                        } else {
                            ++list_in_map_it;
                        }
                    }
                   
                }
                map_it = fileToClientList.find(*it);
                if(map_it != fileToClientList.end() ) {
                    if(map_it->second.size() <= 0) {
                        cout << "List is empty for this file so delete entry for this filename from map"<<endl;
                        fileToClientList.erase(map_it);
                    }
                }
                
            }
        }
        cout << "Deleted " << count << " entries for this sockfd: " << sockfd << endl;
    } else {
        cout << " filename list is empty "<< endl;
    }
    return 0;

}

/*  
 *  ***************************** showing byte wise search packet content ******************
                        0    1    2   3   4  ...............4+i
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                        |msg type | packet|   filename        |
                        |         | length|(i bytes filename) |
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ***************************************************************************************
 * 
 ***************************** showing byte wise sent packet content  ******************
 *                                         NO ERROR                    
                        0    1    2   3   4     5   6 ..................6+i
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-
                        |msg type | packet|filename | filename          |No of ip,port for| ip     | port    | ip     | port    | ...........
                        |         | length|len      |(i bytes filename) |file(2 bytes)    |(4bytes)|(2 bytes)|(4bytes)|(2 bytes)| ...........
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+--+-+-+-+-+-+-+-+-+-+-
 ***************************************************************************************
 * 
 ***************************** showing byte wise sent packet content ******************
                                         ERROR Packet               
                        0    1    2   3   4  ...............4+i
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                        |msg type | packet|   Error Msg       |
                        |         | length|(i bytes error msg)|
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 **************************************************************************************
 */

int ClientHandler::handleSearch(char* resp_buff, int* resp_len) {

    string fileName = "";
    map<string, list<struct client_listen_info> >::iterator it;
    list<struct client_listen_info >::iterator list_it;
    list<struct client_listen_info > clientInfoList;

    int index_recv_buff = 3; // skip msg type , len of msg( 2 bytes)
    int filename_len = 0;

    // Process received search request packet
    filename_len = recv_len - 3;
    fileName.assign((char*) recv_buff + index_recv_buff, filename_len);
    index_recv_buff += filename_len;
    
    cout << fileName << endl;
    it = fileToClientList.find(fileName);
    if (it == fileToClientList.end()) {
        // no file exists with this name
        cout << " file not found " << endl;
        //Send error packet to client
        int index_of_resp = 0;
        memset(resp_buff, 0, size_resp_buff);

        *(uint8_t *) resp_buff = msg_type_err;
        index_of_resp += 1;
        *(uint16_t *) (resp_buff + 1) = htons(index_of_resp);
        index_of_resp += 2;
        string err_msg = "File Not Found";
        sprintf(resp_buff + index_of_resp, "%s", err_msg.c_str());
        index_of_resp += err_msg.length();

        cout << " index of resp " << index_of_resp << endl;
        *resp_len = index_of_resp;
        //Print packet sent in hex
        /*for (int i = 0; i < index_of_resp; i++) {
            printf("%.02x ", resp_buff[i]);
        }*/
        return -1;
    }

    /*- add 1 byte msg type 
        add 2 byte of msg len
     *  add 2 byte of count of no of ip,ports tuples
     *  add 4 byte ip,2 byte port  ( add all)
     */

    int index_of_resp = 0;
    memset(resp_buff, 0, size_resp_buff);

    *(uint8_t *) resp_buff = msg_type_search;
    index_of_resp += 1;

    index_of_resp += 2; // for len of msg

    *(uint16_t *) (resp_buff + index_of_resp) = htons(filename_len);
    index_of_resp += 2;

    memcpy(resp_buff + index_of_resp, fileName.c_str(), filename_len);
    index_of_resp += filename_len;

    clientInfoList = fileToClientList.at(fileName);

    // add no of (ip,port) pair available for this file)
    *(uint16_t *) (resp_buff + index_of_resp) = htons((uint16_t) (clientInfoList.size()));
    index_of_resp += 2;

    //add all (ip,port) tuple)
    for (list_it = clientInfoList.begin(); list_it != clientInfoList.end(); ++list_it) {
        *(uint32_t *) (resp_buff + index_of_resp) = htonl((*list_it).listen_ip);
        index_of_resp += 4;
        *(uint16_t *) (resp_buff + index_of_resp) = htons((*list_it).listen_port);
        index_of_resp += 2;
    }

    // add length of complete packet - it was not added before just the index was incremented to make space for it
    *(uint16_t *) (resp_buff + 1) = htons(index_of_resp);
    *resp_len = index_of_resp;          //set length of response packet

    //Print packet in hex
    /*for (int i = 0; i < *resp_len; i++) {
        printf("%.02x ", resp_buff[i]);
    }*/
    return 0;

}

/*
 * Update time at which ping message received for a client
 */
int ClientHandler::handlePing() {

    last_ping_time = time(NULL);
   return 0;
}

/************************************************************* functions  **************************************************************************************/
// client might be disconnected or no ping received so time out

/*
int handleClientConnectionClosure(ClientHandler * ptr_clientHandler, int sockfd) {

    map<int, ClientHandler>::iterator it;

    ptr_clientHandler->removeEntriesFromfileToClientList();

    close(sockfd);
    it = sockToClient.find(sockfd);
    if (it != sockToClient.end()) {
        sockToClient.erase(it);
    }
    cout << sockToClient.size() << "^^^^^^^^^^^^^^^" << endl;
    return 0;
}
*/
/*
static void * checkHeartbeat(void *args) {

    while (1) {
        cout << "&mtx_sockToClient lock taken " << endl;
        pthread_mutex_lock(&mtx_sockToClient);
        //iterate over map to find last_ping_time of each entry for ClientHAndler in map
        //If current time and last ping time has difference > ping_interval
        //close connection and handle connection closure by doing all that is to be done on connection closure
        map<int, ClientHandler>::iterator it;
        ClientHandler *ptr_clientHandlerObj;

        cout << sockToClient.size() << endl;
        for (it = sockToClient.begin(); it != sockToClient.end(); it++) {
            ptr_clientHandlerObj = &(it->second);
            cout << ptr_clientHandlerObj->last_ping_time << endl;
            if ((time(NULL) - ptr_clientHandlerObj->last_ping_time) > ping_interval) {
                handleClientConnectionClosure(ptr_clientHandlerObj, ptr_clientHandlerObj->sockfd);
                ptr_clientHandlerObj = NULL;
                cout << " closing connection as no ping " << endl;
            }
        }
        pthread_mutex_unlock(&mtx_sockToClient);
        cout << "&mtx_sockToClient lock released " << endl;
        sleep(ping_interval);
    }
}*/

/*************************************************************  main  **************************************************************************************/


int main(int argc, char** argv) {


    int port;
    int server_sockfd = -1;
    char ip[32];
    int ret = -1;
    int new_sockfd = -1;
    int len = -1;
    fd_set master;
    fd_set read_fd;
    fd_set write_fd;

    FD_ZERO(&master); // clear the master and temp sets
    FD_ZERO(&read_fd);
    FD_ZERO(&write_fd);

    Transport transportObj;

    if (argc < 2) {
        cout << argc << endl;
        cout << " Insufficient args" << endl;
        abort();
    }

    memset(ip, 0, 32);
    memcpy(ip, argv[1], 32);
    port = atoi(argv[2]);
    if (port > 0) {
        port = port;
    }

    //create,bind a socket and start listening on it
    server_sockfd = transportObj.createAndBindSocket(ip, port);
    if (server_sockfd < 0) {
        //error
        abort();
    }

    cout << "server started" << endl;

    //add listening socket to set - so that can add it to select to be notified for accept events
    FD_SET(server_sockfd, &master);

    int max_fd = server_sockfd;
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    int i = 0;

    while (1) {

        //copy it to master 
        read_fd = master;
        // keep master as read_fd will be modified at certain select calls
        if (select(max_fd + 1, &read_fd, NULL, NULL, &tv) == -1) {
            //error
            perror("Select()");
            abort();
        }

        //check all fds if any is set check for what it is set and take appropriate action
        for (i = 0; i <= max_fd; i++) {

            if (FD_ISSET(i, &read_fd)) {
                //check if its a listening socket  - accept connection
                if (i == server_sockfd) {

                    struct sockaddr_in client_addr;
                    memset(&client_addr, 0, sizeof (struct sockaddr_in));
                    len = 0;
                    new_sockfd = transportObj.acceptConn(&client_addr, &len);
                    if (new_sockfd < 0) {
                        //failed to  accept connection
                        cout << " error in accept" << endl;
                        continue;
                    }
                    
                    cout << "Connection accepted " << endl;
                    cout << " client ip " << inet_ntoa(client_addr.sin_addr) << endl;
                    cout << " client port " << ntohs(client_addr.sin_port) << endl;
                    cout << " sock fd " << new_sockfd << endl;

                    FD_SET(new_sockfd, &master); // add to master set
                    if (new_sockfd > max_fd) { // keep track of the max
                        max_fd = new_sockfd;
                    }

                    //create client handler object
                    ClientHandler clientHandlerObj;
                    clientHandlerObj.sockfd = new_sockfd;
                    clientHandlerObj.last_ping_time = time(NULL);

                    sockToClient.insert(pair<int, ClientHandler>(new_sockfd, clientHandlerObj));
                    
                } else {

                    //handle read event on it

                    // read data from tcp socket if success then whatever data is there handle to client handler
                    map<int, ClientHandler>::iterator it;
                    ClientHandler* ptr_clientHandlerObj;
                    int len_of_msg = 0;

                    // find clientHandler object for current socket descriptor
                    it = sockToClient.find(i);
                    if (it == sockToClient.end()) {
                        //no such sockfd exists in list
                        cout << "No sockfd found" << endl;
                        continue;
                    }

                    //cout << " sock fd found for this connection " << i << endl;
                    ptr_clientHandlerObj = &(sockToClient.at(i));

                    ret = 0;
                    //receive data in client handler's recv buff 
                    // use index_read as data may be received in chunks
                    ret = transportObj.recvData(i, ptr_clientHandlerObj->recv_buff + ptr_clientHandlerObj->index_read, size_recv_buff - ptr_clientHandlerObj->index_read);
                    if (ret < 0) {
                        perror("recv");
                        // clear everything to be prepared for next packet - do it in here itself
                        // can do it in client also
                        memset(ptr_clientHandlerObj->recv_buff, 0, size_recv_buff);
                        ptr_clientHandlerObj->recv_len = 0;
                        ptr_clientHandlerObj->index_read = 0;
                    } else if (ret == 0) {
                        cout << "connection closed" << endl;
                        //connection closed by client 
                        //remove it from select
                        //remove its entry from all maps
                        FD_CLR(i, &master); // remove from master set
                        ptr_clientHandlerObj->removeEntriesFromfileToClientList();
                        close(ptr_clientHandlerObj->sockfd);

                        map<int, ClientHandler>::iterator it;
                        //erase entry for this sockfd form sockToClient map as connection closed for this
                        it = sockToClient.find(ptr_clientHandlerObj->sockfd);
                        sockToClient.erase(it);
                        ptr_clientHandlerObj = NULL;
                    } else {
                        // data received - process it 

                        //cout << "Receievd " << ret << " bytes" << endl;
                        ptr_clientHandlerObj->index_read += ret;
                        //cout << " index read " << ptr_clientHandlerObj->index_read << endl;

                        // atleast 3 bytes read
                        if (ptr_clientHandlerObj->index_read >= 3) {
                            // fetching length of msg 
                            len_of_msg = ntohs(*(uint16_t*) (ptr_clientHandlerObj->recv_buff + 1));
                            //cout << " len of msg " << len_of_msg << endl;

                            // full packet read - can process now and reset
                            // as suppose he sent 16 bytes but published len of msg as 14 bytes so just get 14 bytes of content and read
                            if (ptr_clientHandlerObj->index_read >= len_of_msg) {

                                ptr_clientHandlerObj->recv_len = len_of_msg;
                                //cout << " recv_len " << ptr_clientHandlerObj->recv_len << endl;

                                char resp_buff[size_resp_buff];
                                int resp_len = -1;

                                if (ptr_clientHandlerObj->dataHandler(resp_buff, & resp_len) < 0) {
                                    //cout << " no data to be sent" << endl;
                                    //no data to be sent
                                } else {
                                    int len_sent = 0;
                                    while (len_sent < resp_len) {

                                        ret = -1;
                                        if ((ret = transportObj.sendData(i, resp_buff + len_sent, resp_len - len_sent)) < 0) {
                                            cout << "error in sending data" << endl;
                                            //need to break
                                            break;
                                        }
                                        cout << "Sent" << ret << "  bytes successfully" << endl;
                                        len_sent += ret;
                                    }
                                }
                                // data read and sent so clear everything to be prepared for next packet - do it in here itself
                                // can do it in client also
                                memset(ptr_clientHandlerObj->recv_buff, 0, size_recv_buff);
                                ptr_clientHandlerObj->recv_len = 0;
                                ptr_clientHandlerObj->index_read = 0;
                            }
                        } else {
                            // not sufficient bytes read - still dont know length of msg to be read
                            // need to read more in the buffer
                        }
                    }

                }
            } else if (FD_ISSET(i, &write_fd)) {
                
              // not handling it
            } else {
                //not handling except fds
                // this when none of the event happened on this fd 
                //so do nothing
            }

        }

        //iterate over map to find last_ping_time of each entry for ClientHAndler in map
        //If current time and last ping time has difference > ping_interval
        //close connection and handle connection closure by doing all that is to be done on connection closure
        map<int, ClientHandler>::iterator it;
        ClientHandler *ptr_clientHandlerObj;

        if (sockToClient.size() > 0) {
            for (it = sockToClient.begin(); it != sockToClient.end();) {
                ptr_clientHandlerObj = &(it->second);

                if ((time(NULL) - ptr_clientHandlerObj->last_ping_time) > ping_interval) {

                    // cout << " removing entry for " << ptr_clientHandlerObj->sockfd << endl;
                    FD_CLR(ptr_clientHandlerObj->sockfd, &master);
                    ptr_clientHandlerObj->removeEntriesFromfileToClientList();
                    close(ptr_clientHandlerObj->sockfd);
                    sockToClient.erase(it++);
                    ptr_clientHandlerObj = NULL;
                
                } else {
                    ++it;
                }
            }
        }

    }
    return 0;
}

