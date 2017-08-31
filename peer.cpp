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
#include <pthread.h>
#include <stdint.h>

using namespace std;
/*
Displays menu on terminal
    1:publish  2:fetch  3:join
User has to enter one of the number to perform the operation

If join is not performed fetch publish can't be done , so user is notified to first join the server
Once the user joins the server
menu displayed is only- 
    1:publish  2:fetch 
As no longer join is required, client maintains it connection permanently with server

For serving file to other peer connection is temporary and it is only till file is served then its closed

For fetching file from other peer connection is temporary, it is only till file is being fetched , at the end its closed

TCP is used for all types of communication
*/

/*  ************************************ ARCHITECTURE ***************************************************
 * MAIN THREAD - 
 *          It prints the menu on terminal and take action based on user input
 *          It creates AS_Server THREAD at beginning
 *          It creates AS_Client_For_Main_Server Object when user wishes to join central server
 *          All operations with main server - like publish , search , join are done from this thread based on user input for menu displayed
 *          It supports a separate search operation from main server too , as it is not required separately but its to be done just before fetch 
 *          so part of code has been commented.
 *          It accepts connection , receives packet.
 *          If sends out packet for search operation and receives data from server , calls appropriate handler to process this received data
 *          Once processed successfully , it fetches data from another server by initiating a separate  thread that fetches file parallely
 *          So many files can be fetches together
 *          A file can be fetches and another can be published together
 *          A file can be fetched and peer can upload another file to some other peer concurrently
 *         Thus all these operations can be done concurrently
 *  
 * 
 * AS_Server THREAD -
 *          A separate thread is initiated from main thread at beginning
 *          It acts as server for other peers who wish to download files from it
 *          It runs all the time - for each request from a peer it starts a thread after accepting connection
 *          This new thread is used to serve file .Thread ends as soon as serving file is done.
 *           
 * 
 * AS_Client_For_Fetch THREAD -
 *          This thread is initiated from main thread.
 *          It is used to fetch file from another peer
 *  
 * 
 * ping_thread - 
 *          This thread sends ping message as a keep alive to server periodically
 *          It expects no response from server,message is just 1 way
 */

//defines buffer sizes for various buffers used
#define sendbuff_size_asClient 1024
#define recvbuff_size_asClient 1024

#define size_recv_buff 1024
#define size_resp_buff 1024
#define size_file_send_buff 1024
#define size_file_recv_buff 1024

//defines msg types
#define msg_type_publish 0
#define msg_type_search 1
#define msg_type_fetch 2
#define msg_type_ping 3
#define msg_type_err 10

//defines time interval at which it will send a ping to server
#define ping_interval 20
#define file_recv_TO 15

//structure to store ip,port of peers which have published a file - this is stored in map against a filename 
struct peer_info {
    int peer_ip;
    int peer_port;
};

//Stores ip,port at which peer behaves as server to serve file to other peers
struct as_server_listen_info {
    char server_ip[32];
    uint16_t server_port;
};


/* This map is used by different threads so a lock is used to access it
 * It is used when peers acts as client and searches the server to get (ip,port ) list for a file
 * It is also used when peer behaves as client to another peer and requests a file in fetchFile in separate thread
 * 
 */
pthread_mutex_t mtx_fileToPeerList;
/* This map is used to keep track of (ip,port) pairs of all peers that have a file
 * This is maintained against a filename as key
 * When this peer searches server for a file , whatever data it gets , it places it in this map
 * During fetch it searches this map 
 * If entry for a file is found -it gets the list of (ip,port) of peers that have this file
 * It attempts connecting to one of those peer , once connected it fetches file
 * Once connects to at least 1 peer it doesn't attempt connection with others
 */
map<string, list<struct peer_info> > fileToPeerList;

/*
 * Lock is sued to access this map 
 * As same map is used by different threads
 * When a peer behaves as a server during handleFileReq - it checks this list
 * When a peer behaves as a client during publish - in main thread it accesses this list
 */
pthread_mutex_t mtx_filesPublishedList;
/*This list is used to keep track of files published by a client 
 * If a some other peer requests a file , this peer checks it in this list , if there
 * that means it has published so it serves else closes connection to show that it doesn't have this file 
 */
list<string> filesPublishedList;

class Transport {
public:
    int client_listen_ip;
    int client_listen_port;
    int sockfd;

    Transport() {

    }

    int createSocket();
    int bindSocket(const char* ip, uint16_t port);
    int setListen();
    int sendData(int sockfd, char* sendbuff, int len);
    int recvData(int sockfd, char *recvbuff, int max_len);
    int acceptConn(struct sockaddr_in* client_addr, int* len);
    int connectTo(int sockfd, const char* server_ip, uint16_t server_port);
};

// should close connection to client after sending file  - so closes connection and ends thread

class AS_Server {
public:
    //Transport transportObj;

    AS_Server() {

    }

    static void * startServer(void* args);
    int dataHandler(char* resp_buff, int* resp_len);
    int handleFetch();
    int closeConnection();
};

class Serve_File {
public:

    Serve_File() {

    }

    static void * handleFileReq(void *);
};

class AS_Client_For_Fetch {
public:

    AS_Client_For_Fetch() {

    }

    static void * fetchFile(void *args);
};

class AS_Client_For_Main_Server {
public:

    Transport transportObj;
    int connected; // TO mark if it is connected to server or not 0 - shows disconnected , 1-shows connected

    AS_Client_For_Main_Server() {
        connected = 0;

    }

    static void * pingServer(void *args);
    int search(string filename);
    int publish(const char* listen_ip, uint16_t listen_port, list<string> filenameList);
    int connectToServer(const char* server_ip, int server_port);
    int recvData(char *buff, int len);

};

/****************************************************************************************************************************************************
                                                    TRANSPORT
 ****************************************************************************************************************************************************/

int Transport::createSocket() {
    //create listening socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        //error 
        perror("Socket creation failed");
        close(sockfd);
        return -1;
    }
    return sockfd;
}

int Transport::bindSocket(const char* ip, uint16_t port) {
    int ret = -1;
    struct sockaddr_in serv_addr;

    //make structure for server addr
    memset(&serv_addr, 0, sizeof (serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = inet_addr(ip);

    //bind socket to port 
    ret = bind(sockfd, (struct sockaddr *) &serv_addr, sizeof (struct sockaddr));
    if (ret < 0) {
        //bind failed
        perror("bind failed");
        close(sockfd);
        return -1;
    }
    return 0;
}

int Transport::setListen() {


    int ret = -1;
    // set server to listening mode
    ret = listen(sockfd, 50);
    if (ret < 0) {
        perror("listen failed");
        return -1;
    }
    return 0;
}

/* It is used to send data on transport 
 * if success returns no of bytes sent 
 * if failure returns <0 
 */
int Transport::sendData(int sockfd, char* sendbuff, int len) {

    return send(sockfd, sendbuff, len, 0);

}

//if success returns no of bytes recvd if failure returns <0 

int Transport::recvData(int sockfd, char *recvbuff, int max_len) {

    return recv(sockfd, recvbuff, max_len, 0);

}

//returns new sock fd

int Transport::acceptConn(struct sockaddr_in* client_addr, int* len) {

    int ret = -1;

    ret = accept(sockfd, (struct sockaddr *) client_addr, (socklen_t *) len);
    if (ret < 0) {
        perror("accept");
        return -1;
    }
    return ret;
}

int Transport::connectTo(int sockfd, const char* server_ip, uint16_t server_port) {

    struct sockaddr_in serv_addr;

    memset(&serv_addr, 0, sizeof (serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = server_port;
    serv_addr.sin_addr.s_addr = inet_addr(server_ip);


    if (connect(sockfd, (struct sockaddr *) &serv_addr, (socklen_t) (sizeof (struct sockaddr))) < 0) {
        perror("Couldnt conenct ");
        return -1;
    }
    return 0;
}

/****************************************************************************************************************************************************
                                                       AS_Client_For_Main_Server
 ****************************************************************************************************************************************************/

int AS_Client_For_Main_Server::connectToServer(const char* server_ip, int server_port) {

    if (transportObj.createSocket() < 0) {
        cout << " socket creation failed" << endl;
        return -1;
    }

    if (transportObj.connectTo(transportObj.sockfd, server_ip, server_port) < 0) {
        cout << " couldn't connect to server " << endl;
        return -2;
    }
    connected = 1;
    return 0;
}

//Its a thread that will keep sending ping message as heartbeat to server periodically

/*  
 ***************************** showing byte wise ping packet content ************************
                                    0   1    2   3   4  5   6   7   8  
                                    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                                    |msg type | packet|   PING      |
                                    |         | length|             |
                                    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ***********************************************************************************************/
void * AS_Client_For_Main_Server::pingServer(void *args) {

    int send_len;
    char send_buff[size_resp_buff];
    int sockfd;
    int sent = -1;

    Transport transportObj;

    sockfd = *((int *) args);
    transportObj.sockfd = sockfd;

    while (1) {

        send_len = 0;
        memset(send_buff, 0, size_resp_buff);

        *(uint8_t *) send_buff = msg_type_ping;
        send_len += 1;
        *(uint16_t *) (send_buff + 1) = htons(send_len);
        send_len += 2;
        string ping_msg = "PING";
        sprintf(send_buff + send_len, "%s", ping_msg.c_str());
        send_len += ping_msg.length();

        sent = -1;
        sent = transportObj.sendData(sockfd, send_buff, send_len);
        if (sent < 0) {
            cout << "Failed to send ping message " << endl;
        }
        //Sent success prints packet in hex
        /*
        else {
            for (int i = 0; i < send_len; i++) {
                printf("%.02x ", send_buff[i]);
            }
        }*/
        sleep(ping_interval);
        //FOR TESTING PING
        //sleep(1000);
    }
    pthread_exit(NULL);

}

/*  
 ***************************** showing byte wise publish packet content ************************
                        0   1    2   3   4  5   6   7   8   9   10   11   12 ....... 12+i 
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                        |msg type | packet|   Listen ip  |Listen  |Length of| filename  |
                        |         | length|   (4 bytes)  |port    | filename|(i bytes)  |
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ***********************************************************************************************/

int AS_Client_For_Main_Server::publish(const char* listen_ip, uint16_t listen_port, list<string> filenameList) {

    int index = 0;
    char send_buff[sendbuff_size_asClient];
    list<string>::iterator it;
    int fileExists = 0; // To check if a file is published by this client or not
    // If it is already in filePublishedList then don't add it 


    memset(send_buff, 0, sendbuff_size_asClient);

    //add msg type
    send_buff[0] = msg_type_publish;
    index += 1;

    index += 2; // to account for 2 bytes of len of msg

    //add listen ip
    *(uint32_t*) (send_buff + index) = inet_addr(listen_ip);
    index += 4;

    //add listen port
    *(uint16_t*) (send_buff + index) = htons(listen_port);
    index += 2;

    //add all filenames to publish in buff - currently supporting just 1 file
    //Can later integrate to publish more than 1 file at a time
    for (it = filenameList.begin(); it != filenameList.end(); ++it) {
        *(uint16_t*) (send_buff + index) = htons((*it).length());
        index += 2;
        sprintf(send_buff + index, "%s", (*it).c_str());
        index += (*it).length();
    }

    // add len of msg 
    *(uint16_t*) (send_buff + 1) = htons(index);

    if (transportObj.sendData(transportObj.sockfd, send_buff, index) < 0) {
        cout << " couldnt send data to server " << endl;
        //cout << " publish failed " << endl;
        return -1;
    }

    /*
     * For all file published successfully add it to list filesPublishedList
     */

    pthread_mutex_lock(&mtx_filesPublishedList);
    //cout << " mtx_fileList lock taken " << endl;

    //check if list has this file else add it
    list<string>::iterator it_fileList;

    //Do it for all files - currently supporting just 1 file
    for (it = filenameList.begin(); it != filenameList.end(); ++it) {

        for (it_fileList = filesPublishedList.begin(); it_fileList != filesPublishedList.end(); ++it_fileList) {
            if ((*it).compare(*it_fileList) == 0) {
                //skip adding it 
                // cout << " this file is already in fileList" << endl;
                fileExists = 1;
                break;
            }
        }
        // file wasn't found so add it in list
        if (!fileExists) {
            filesPublishedList.push_front(*it);
            //cout << *it << " added to filesPublishedList" << endl;
        }
    }
    pthread_mutex_unlock(&mtx_filesPublishedList);
    // cout << " mtx_fileList lock released " << endl;
    return 0;
}

/*
 * Search the server for this filename
 */

/*  
 ***************************** showing byte wise search packet content ******************
                        0    1    2   3   4  ...............4+i
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                        |msg type | packet|   filename        |
                        |         | length|(i bytes filename) |
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ***************************************************************************************/

int AS_Client_For_Main_Server::search(string filename) {

    int index = 0;
    char send_buff[sendbuff_size_asClient];
    memset(send_buff, 0, sendbuff_size_asClient);

    //add msg type 
    *(uint8_t*) (send_buff) = msg_type_search;
    index += 1;

    index += 2; // to account for 2 bytes of len of msg

    sprintf(send_buff + index, "%s", filename.c_str());
    index += filename.length();

    *(uint16_t*) (send_buff + 1) = htons(index);
    //Prints packet to be sent in hex
    /*
    for (int i = 0; i < index; i++) {
        printf("%.02x ", send_buff[i]);
    }
     * */


    if (transportObj.sendData(transportObj.sockfd, send_buff, index) < 0) {
        cout << " couldnt send data to server " << endl;
        cout << " search failed " << endl;
        return -1;
    }
    return 0;
}

/*  
 *********************************************** showing byte wise received packet content  *****************************
 *                                                            NO ERROR                    
                        0    1    2   3   4  ...............4+i
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                        |msg type | packet|   filename        | No of peers              | peer ip|peer port|peer ip|peer port| ....
                        | (1)     | length|(i bytes filename) | having this file (2Bytes)| 4bytes | 2bytes  |4bytes | 2bytes  | ...
                        +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 **********************************************************************************************************************
 * 
 ************************************************ showing byte wise received packet content *******************************
                                                                 ERROR Packet               
                                                0    1    2   3   4  ...............4+i
                                                +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                                                |msg type | packet|   Error Msg       |
                                                | (10)    | length|(i bytes error msg)|
                                                +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 ****************************************************************************************************************************
 */

/*
 * Handles data received from server after search request was sent ....
 * If msg type is error - it shows some error occurred at server or file wasn't found 
 * If msg type is search - details successfully received for file so get ip ,port received and store it in map
 */
int AS_Client_For_Main_Server::recvData(char* recv_buff, int recv_len) {

    // process data received from server
    int msg_type = -1;
    int len_of_msg = -1;
    int len_filename = 0;
    string filename = "";
    int count_peers = 0;
    list < struct peer_info> peer_list;
    map<string, list < struct peer_info> >::iterator it;
    int i, index_recv_buff = 0;


    // to print packet in hex
    /* for (i = 0; i < recv_len; i++) {
         printf("%.02x ", recv_buff[i]);
     }
     */

    //Fetch details from packet
    //msg type
    msg_type = *(uint8_t*) (recv_buff + index_recv_buff);
    index_recv_buff += 1;

    if (msg_type == msg_type_err) {
        cout << "Couldn't get details for this search from server " << endl;
        cout << "Error packet received " << endl;
        return -1;
    }

    //len of msg
    len_of_msg = ntohs(*(uint16_t*) (recv_buff + index_recv_buff));
    index_recv_buff += 2;

    //filename and its length
    len_filename = ntohs(*(uint16_t*) (recv_buff + index_recv_buff));
    index_recv_buff += 2;

    filename.assign((char*) recv_buff + index_recv_buff, len_filename);
    index_recv_buff += len_filename;

    //No of peers that have published this file
    count_peers = ntohs(*(uint16_t*) (recv_buff + index_recv_buff));
    index_recv_buff += 2;

    /*  
     * take lock and then do all activity
     */
    pthread_mutex_lock(&mtx_fileToPeerList);

    it = fileToPeerList.find(filename);
    //To store value returned by insert() in map 
    pair < map<string, list < struct peer_info> >::iterator, bool> ret;

    /*
     * If entry exists for this file  , then this entry is stale as with latest search from server we have updated data
     * So remove existing entry
     */
    if (it != fileToPeerList.end()) {
        //cout << "Entry exists remove entry from map and store this updated info" << endl;
        fileToPeerList.erase(it);
    }

    //extract ip,port for peers
    for (i = 0; i < count_peers; i++) {

        struct peer_info peerDetails;
        peerDetails.peer_ip = ntohl(*(uint32_t*) (recv_buff + index_recv_buff));
        index_recv_buff += 4;
        //cout << " Peer ip for file search " << peerDetails.peer_ip << endl;
        peerDetails.peer_port = ntohs(*(uint16_t*) (recv_buff + index_recv_buff));
        index_recv_buff += 2;
        //cout << " Peer port for file search " << peerDetails.peer_port << endl;

        //make entry in list for the filename
        peer_list.push_front(peerDetails);
    }

    /* Insert this new entry (filename , list of (ip,port) ) in map
     */
    ret = fileToPeerList.insert(pair<string, list < struct peer_info> >(filename, peer_list));
    if (ret.second == true) {
        //cout << "inserted " << (ret.first)->first << endl;
    } else {
        cout << "Couldn't insert" << endl;
        //release lock
        pthread_mutex_unlock(&mtx_fileToPeerList);
        //cout << "mtx_fileToPeerList lock released " << endl;
        return -1;
    }

    //release lock
    pthread_mutex_unlock(&mtx_fileToPeerList);
    //cout << "mtx_fileToPeerList lock released " << endl;

    return 0;
}

/****************************************************************************************************************************************************
                                                       AS_SERVER
 ****************************************************************************************************************************************************/

void * AS_Server::startServer(void* args) {

    int server_sockfd = -1;
    int new_sockfd = -1;
    int len = -1;
    //char filename[100];
    struct as_server_listen_info* server_info;
    Transport transportObj;

    //ill use index till 1024 and then loop back 
    // reuse index in next cycle
    pthread_t as_server__to_serve_file_thread[1024];
    int thread_count = 0;

    server_info = (struct as_server_listen_info*) args;

    //create and bind socket
    server_sockfd = transportObj.createSocket();
    if (server_sockfd < 0) {
        //error
        cout << "Socket creation failed" << endl;
        abort();
    }

    if (transportObj.bindSocket(server_info->server_ip, server_info->server_port) < 0) {
        cout << "  binding failed " << endl;
        //error
        abort();
    }

    if (transportObj.setListen() < 0) {
        cout << "  listen failed " << endl;
        //error
        abort();
    }

    cout << "server started at " << server_info->server_port << endl;

    while (1) {
        struct sockaddr_in client_addr;
        memset(&client_addr, 0, sizeof (struct sockaddr_in));
        len = 0;

        new_sockfd = transportObj.acceptConn(&client_addr, &len);
        if (new_sockfd < 0) {
            //failed to  accept connection
            cout << "Failed to accept connection" << endl;
            continue;
        }

        cout << "Connection accepted " << endl;
        cout << "client addr " << inet_ntoa(client_addr.sin_addr) << endl;
        Serve_File serveFileObj;
        //start a new thread for this connection 
        //Thread will handle sending file requested 
        // use same transport object and sockfd will be new_sockfd
        pthread_create(&as_server__to_serve_file_thread[thread_count], NULL, serveFileObj.handleFileReq, (void *) (&new_sockfd));
        thread_count++;
        //as using 1024 identifiers for thread
        if (thread_count > 1024) {
            thread_count = 0;
        }
        sleep(2);
    }
}

/****************************************************************************************************************************************************                                              
                                                     Serve_File
 ****************************************************************************************************************************************************/
 /***************************** showing byte wise received packet content ******************
                                                        
                                0    1    2   3   4  ...............4+i
                                +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                                |msg type | packet|   File name       |
                                |   (2 )  | length|(i bytes error msg)|
                                +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 **************************************************************************************
 */
/* Gets socket descriptor for a client whose connection was accepted and will now send request to fetch a file
 * This socket descriptor was passed when this thread was created after accepting client's connection
 * It used this socksd to communicate with client(another peer) 
 */
void* Serve_File::handleFileReq(void* args) {


    int ret;
    int index_read = 0;
    Transport transportObj;
    char recv_buff[size_recv_buff];     //This will store packet received from peer for file request
    int len_of_msg = 0;
    int msg_type;
    string filename;
    int len_of_filename = 0;

    char resp_buff[size_resp_buff];
    int errFlag = 0;



    int socksd = *((int *) args);
    transportObj.sockfd = socksd;
    memset(recv_buff, 0, size_recv_buff);

    //keep reading till sufficient bytes read
    while (1) {
        ret = transportObj.recvData(socksd, recv_buff + index_read, size_recv_buff - index_read);
        if (ret < 0) {
            //error in recv 
            perror("recv");
            errFlag = 1;
            break;
        } else if (ret == 0) {
            cout << "connection closed" << endl;
            close(socksd); // close connection
            errFlag = 1;
            break;
        } else {
            // data recv
            //cout << " data recvd " << ret << " bytes " << endl;
            index_read += ret;
            if (index_read >= 3) {
                // fetching length of msg 
                len_of_msg = ntohs(*(uint16_t*) (recv_buff + 1));
                //cout << " len of msg " << len_of_msg << endl;

                // full packet read - can process now and reset
                // as suppose he sent 16 bytes but published len of msg as 14 bytes so just get 14 bytes of content and read
                if (index_read >= len_of_msg) {
                    break;
                }
            }
        }
    }

    if (errFlag) {
        cout << " error occurred " << endl;
        close(socksd);
        pthread_exit(NULL);
    }

    // to print packet in hex
    /*
    for (i = 0; i < index_read; i++) {
        printf("%.02x ", recv_buff[i]);
    }
     */
    msg_type = *(uint8_t*) recv_buff;
    //cout << "msg type  " << msg_type << endl;

    // find out file name and serve that file
    if (msg_type != msg_type_fetch) {
        cout << " wrong msg type in request " << endl;
        close(socksd);
        pthread_exit(NULL);
    }

    len_of_filename = len_of_msg - 3;
    filename.assign(recv_buff + 3, len_of_filename);

    //TODO -check if this filename is in map of published files or not
    pthread_mutex_lock(&mtx_filesPublishedList);
    //cout << "mtx_filesPublishedList lock taken " << endl;
    errFlag = 1;
    list<string>::iterator it;
    for (it = filesPublishedList.begin(); it != filesPublishedList.end(); it++) {
        cout << "Filename in list " << (*it) << endl;
        if (strcmp((*it).c_str(), filename.c_str()) == 0) {
            //not an error
            errFlag = 0;
            break;
        }
    }

    if (errFlag) {
        //filename not in published list - error 
        cout << "Filename requested for fetch is not in published list " << endl;
        pthread_mutex_unlock(&mtx_filesPublishedList);
        //cout << "mtx_filesPublishedList lock released " << endl;
        close(socksd);
        pthread_exit(NULL);
    }

    pthread_mutex_unlock(&mtx_filesPublishedList);
    //cout << "mtx_filesPublishedList lock released " << endl;

    FILE *filehandle = fopen(("p2p-files/" + filename).c_str(), "rb");
    if (filehandle == NULL) {
        cout << " no such file found " << endl;
        close(socksd);
        pthread_exit(NULL);
    }

    fseek(filehandle, 0, SEEK_END);
    long filesize = ftell(filehandle);

    rewind(filehandle);
    if (filesize == EOF) {
        cout << " end of file " << endl;
        close(socksd);
        fclose(filehandle);
        pthread_exit(NULL);

    }

    memset(resp_buff, 0, size_resp_buff);
    *(uint32_t*) (resp_buff) = htonl(filesize);
    if (send(socksd, resp_buff, 32, 0) < 0) {
        cout << " failed to send " << endl;
        close(socksd);
        fclose(filehandle);
        pthread_exit(NULL);
    }

    if (filesize <= 0) {
        cout << " filesize is 0 nothing to send " << endl;
        close(socksd);
        fclose(filehandle);
        pthread_exit(NULL);

    }

    do {
        size_t num;
        if (filesize < size_resp_buff) {
            num = filesize;
        } else {
            num = size_resp_buff;
        }
        num = fread(resp_buff, 1, num, filehandle);
        if (num < 1) {
            cout << " couldnt read file" << endl;
            close(socksd);
            fclose(filehandle);
            pthread_exit(NULL);
        }
        int sent = send(socksd, resp_buff, num, 0);
        if (sent < 0) {
            cout << " failed to send " << endl;
            close(socksd);
            fclose(filehandle);
            pthread_exit(NULL);
        }
        // cout << " sent " << sent << endl;
        filesize -= sent;
    } while (filesize > 0);

    //cout << " succesfully sent" << endl;
    fclose(filehandle);
    close(socksd);
    //cout << " connection closed after sending " << endl;
    pthread_exit(NULL);

}

/****************************************************************************************************************************************************
                                                       AS_CLIENT_For_Fetch
 ****************************************************************************************************************************************************/
//Takes filename to be fetched from argument passed when thread was created

void * AS_Client_For_Fetch::fetchFile(void* args) {

    string filename;
    int sockfd;
    char recv_buff[size_file_recv_buff];
    int read = -1;
    list < struct peer_info> peer_list;
    list < struct peer_info>::iterator list_it;
    map<string, list < struct peer_info> >::iterator it;
    struct in_addr paddr;
    char *strAdd2;
    int connected = 0;
    struct timeval sock_timeout;

    Transport transportObj;

    filename = *((string*) args);

    //check map if it has this file name if yes find out list of ip port 
    // try connecting to those when connected 
    // take lock and then access map

    pthread_mutex_lock(&mtx_fileToPeerList);
    //cout << "mtx_fileToPeerList lock taken" << endl;

    /* For test
    for (it = fileToPeerList.begin(); it != fileToPeerList.end(); ++it) {
        cout << " iterating over map key " << it->first << endl;
    }
     */
    it = fileToPeerList.find(filename);

    if (it == fileToPeerList.end()) {
        cout << "No information about this file . Not in our list " << endl;
        pthread_mutex_unlock(&mtx_fileToPeerList);
        //cout << "mtx_fileToPeerList lock released" << endl;
        pthread_exit(NULL);
    }

    // get the list of ip port for this file
    peer_list = it->second;

    for (list_it = peer_list.begin(); list_it != peer_list.end(); list_it++) {

        //cout << " ip " << list_it->peer_ip << endl;
        //cout << "port " << list_it->peer_port << endl;

        //Recvd ip in binary but here storing as ascii
        paddr.s_addr = htonl(list_it->peer_ip);

        strAdd2 = inet_ntoa(paddr);
        //cout << " ip after conversion " << strAdd2 << endl;

        if ((sockfd = transportObj.createSocket()) < 0) {
            cout << " Couldn't create socket to fetch file from server" << endl;
            close(sockfd);
            // pthread_exit(NULL);
            continue;
        }

        /* USing a receive time out during fetchFile as we peer might request another peer for a file -
         * another might be busy 
         * or doesn't have file
         * or closes connection because of any reason
         * So if file not received within a given time close the connection
         * Keeping this time large intitially - can change it later
         */
        sock_timeout.tv_sec = file_recv_TO;
        sock_timeout.tv_usec = 0;
        if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*) &sock_timeout, sizeof (sock_timeout)) == -1) {
            cout << "Failed to set recv timeout on socket" << endl;
            close(sockfd);
            continue;
        }

        if (transportObj.connectTo(sockfd, strAdd2, htons(list_it->peer_port)) < 0) {
            cout << "Couldn't connect to server to fetch file" << endl;
            close(sockfd);
            continue;
        }
        cout << " connected to server " << endl;
        //Connected to atleast 1 server with this file 
        connected = 1;
        break;
    }

    pthread_mutex_unlock(&mtx_fileToPeerList);
    // cout << "mtx_fileToPeerList lcok released " << endl;

    if (!connected) {
        cout << " Couldn't connect to any server to fetch file " << endl;
        close(sockfd);
        pthread_exit(NULL);
    }

    //Send data to server giving the filename it wants to fetch
    int index = 0;
    char send_buff[size_file_send_buff];
    int ret = -1;

    memset(send_buff, 0, size_file_send_buff);
    *(uint8_t*) (send_buff) = msg_type_fetch;
    index += 1;
    index += 2; // to account for 2 bytes of len of msg

    sprintf(send_buff + index, "%s", filename.c_str());
    index += filename.length();


    *(uint16_t*) (send_buff + 1) = htons(index);
    ret = transportObj.sendData(sockfd, send_buff, index);

    //Print in hex
    /*
    for (int i = 0; i < index; i++) {
        printf("%.02x ", send_buff[i]);
    }
    cout << "sent " << ret << endl;
     * */

    FILE *filehandle = fopen(("p2p-files/" + filename).c_str(), "wb");
    if (filehandle == NULL) {
        cout << "Couldn't get file handle " << endl;
        close(sockfd);
        pthread_exit(NULL);
    }


    uint32_t filesize;
    // read 32 bytes to get length of file
    memset(recv_buff, 0, size_file_recv_buff);
    read = transportObj.recvData(sockfd, recv_buff, 32);
    if (read < 0) {
        cout << " error in reading " << endl;
        close(sockfd);
        fclose(filehandle);
        pthread_exit(NULL);
    } else if (read == 0) {
        cout << " connection closed " << endl;
        /***************exiting*******************/

        fclose(filehandle);
        close(sockfd);
        pthread_exit(NULL);
    } else {
        //cout << "Received data : " << read << " bytes " << endl;
        filesize = ntohl(*(uint32_t*) recv_buff);
        //cout << " filesize  " << filesize << endl;
    }

    if (filesize <= 0) {
        cout << " No scuh file found " << endl;
        fclose(filehandle);
        close(sockfd);
        pthread_exit(NULL);
    }

    /*Once length received get the full file 
     *   Length was needed to know how much data is to be read
     * Read file size bytes
     */
    do {

        //find lower of 2 sizes
        size_t num;
        if (filesize < size_file_recv_buff) {
            num = filesize;
        } else {
            num = size_file_recv_buff;
        }

        memset(recv_buff, 0, size_file_recv_buff);
        read = transportObj.recvData(sockfd, recv_buff, num);
        if (read < 0) {
            cout << " error in reading " << endl;
            fclose(filehandle);
            close(sockfd);
            pthread_exit(NULL);
        } else if (read == 0) {

            cout << " connection closed " << endl;
            /***************exiting*******************/
            fclose(filehandle);
            close(sockfd);
            pthread_exit(NULL);

        } else {
            // cout << " data received : " << read << " bytes " << endl;

            //Write the data just received in the file
            int offset = 0;
            do {
                size_t written = fwrite(&recv_buff[offset], 1, num - offset, filehandle);
                if (written < 1) {
                    cout << " failed to write " << endl;
                    close(sockfd);
                    fclose(filehandle);
                    pthread_exit(NULL);
                }
                offset += written;
            } while (offset < read);
            filesize -= read;
            // cout << " rem file size " << filesize << endl;
        }
    } while (filesize > 0);

    cout << " file copied " << endl;
    close(sockfd);
    fclose(filehandle);
    pthread_exit(NULL);
}

/****************************************************************************************************************************************************
                                                        main
 ****************************************************************************************************************************************************/

int main(int argc, char** argv) {

    AS_Client_For_Main_Server as_clientObj;
    int server_port;
    char server_ip[32];
    int listen_port;
    char listen_ip[32];
    int ret = -1;
    pthread_t as_server_thread;
    pthread_t as_client_for_fetch_thread;
    pthread_t ping_thread;

    int option = -1;
    int noOfFilesToPublish = 0;
    string filename;

    AS_Client_For_Fetch as_client_for_fetchObj;     //Object of class that handles fetching file from other peer
    list<string> filenameToPublish;                 //If user wants to publish multiple files together this will be used to store
                                                    // filenames of all such files.
                                                    //Currently just 1 file support at a time is there - so this is for later use

    cout << "No of arguments received from command line: " <<argc << endl;
    if (argc < 4) {
        // insufficient cl args
        cout << " Insufficeint args" << endl;
        abort();
    }

    memset(server_ip, 0, 32);
    memcpy(server_ip, argv[1], 32);

    cout << "Main server ip: " << server_ip << endl;
    server_port = htons(atoi(argv[2]));

    memset(listen_ip, 0, 32);
    memcpy(listen_ip, argv[3], 32);

    cout << "My listen ip: " << listen_ip << endl;
    listen_port = atoi(argv[4]);

    AS_Server as_serverObj;
    struct as_server_listen_info server_info;
    memset(&server_info, 0, sizeof (struct as_server_listen_info));

    memcpy(server_info.server_ip, listen_ip, sizeof (listen_ip));
    server_info.server_port = listen_port;

    //Create a separate thread that will act as server for other peers
    pthread_create(&as_server_thread, NULL, as_serverObj.startServer, (void *) &server_info);

    cout << "as_server_thread thread created  " << endl;


    while (1) {
        if (!as_clientObj.connected) {
            cout << "Enter number for the operation to be performed \n 1:publish \n 2:fetch \n 3:join" << endl;
        } else {
            cout << "Enter number for the operation to be performed \n 1:publish \n 2:fetch" << endl;
        }

        cin >> option;
        switch (option) {
            case 1:
                //Publish
                if (!as_clientObj.connected) {
                    printf("Haven't joined server , first join by connecting to server try option 3 first\n");
                    continue;
                }
                filenameToPublish.clear();
                cout << "Enter name of file to be published" << endl;
                noOfFilesToPublish = 1;
                //Keeping it as list as later will integrate feature to publish more than 1 file at a time
                for (int i = 0; i < noOfFilesToPublish; i++) {
                    cin >> filename;
                    filenameToPublish.push_front(filename);
                }

                if (as_clientObj.publish(listen_ip, listen_port, filenameToPublish) < 0) {
                    cout << " publish failed " << endl;
                    continue;
                }
                cout << "publish success " << endl;

                break;
            case 2:
                //Fetch and Search
                //Search
                printf("Enter filename to be fetched \n");
                cin >> filename;
                if (!as_clientObj.connected) {
                    printf("Haven't joined server , first join by connecting to server try option 3 first\n");
                    continue;
                }

                if (as_clientObj.search(filename) < 0) {
                    cout << "Search failed " << endl;
                    continue;
                }
                cout << "Search msg sent success " << endl;

                char recv_buff[recvbuff_size_asClient];
                memset(recv_buff, 0, recvbuff_size_asClient);
                ret = -1;
                if ((ret = as_clientObj.transportObj.recvData(as_clientObj.transportObj.sockfd, recv_buff, recvbuff_size_asClient)) < 0) {
                    cout << " read error " << endl;
                    continue;
                }
                //cout << ret << " bytes read from server " << endl;
                if (as_clientObj.recvData(recv_buff, ret) < 0) {
                    cout << "Error response received for searched filename - Search failed " << endl;
                    continue;
                }
                //Fetch

                pthread_create(&as_client_for_fetch_thread, NULL, as_client_for_fetchObj.fetchFile, (void *) (&filename));
                cout << " thread for fetch file created success " << endl;
                break;

            case 3:
                //join
                if (!as_clientObj.connected) {
                    if (as_clientObj.connectToServer(server_ip, server_port) < 0) {
                        cout << " connection to server failed " << endl;
                        continue;
                        // abort();
                    }
                    cout << "connected to server " << endl;
                    pthread_create(&ping_thread, NULL, as_clientObj.pingServer, (void *) (&as_clientObj.transportObj.sockfd));
                }
                break;
                /*  case 4:
                      //Search
                      if (!connectedToServer) {
                          printf("Haven't joined server , first join by connecting to server try option 3 first\n");
                          continue;
                      }
                      printf("Enter name of file to be searched \n");
                      filename = "";
                      cin >> filename;

                      if (as_clientObj.search(filename) < 0) {
                          cout << " search failed " << endl;
                      }
                      cout << "Search msg sent success " << endl;

                      char recv_buff[recvbuff_size_asClient];
                      memset(recv_buff, 0, recvbuff_size_asClient);
                      ret = -1;

                      if ((ret = as_clientObj.transportObj.recvData(as_clientObj.transportObj.sockfd, recv_buff, recvbuff_size_asClient)) < 0) {
                          cout << " read error " << endl;
                          continue;
                      }
                      cout << ret << " bytes read from server " << endl;
                      as_clientObj.recvData(recv_buff, ret);
                      break;
                 */
            default:
                printf("Inavalid option ! try again \n");
        }

        sleep(1);
    }
    return 0;
}
