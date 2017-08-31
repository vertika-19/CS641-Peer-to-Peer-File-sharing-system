Directory name - 163050007
Untar it

All source files are in c++

It has following files -
	peer.cpp
	server.cpp
	peer executable file
	server executable file
	makefile
	README.txt

How to use makefile to create peer and server executables
	make clean
	make

server.cpp
	It has the code for server.
	How to execute it -
		If server's ip is 127.0.0.1 and port 2000.

		./server 127.0.0.1 2000
		This will start the server at port 2000.

peer.cpp
	It has the code for peers.
	Different instances of this will represent different peers.
	For each client there is a separate folder named p2p-files : It has files it has published or fetched.
	When publishing peer assumes path for file as p2p-files/filename.
	When fetching a file peer assumes path to store it is p2p-files/filename.

	How to execute it -
		If server's ip is 127.0.0.1 and port 2000.
		If peer will serve file to other peers at address 127.0.0.1 3000.

		./peer 127.0.0.1 2000 127.0.0.1 3000

		This will start a server at port 3000 where other peers can contact it for file.

How to create different instances of peers
	Create separate folder like peer1(folder name can be anything).
	Put peer executable file in it.
	Create a folder p2p-files inside this directory peer1 where this peer will keep its fetched and published files.
	Now peer can be executed using executable (use command as described above)

peer executable file - 
	Executable file for peer

server executable file -
	Executable file for server
