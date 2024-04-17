To launch frontend servers: 
First, run the loadbalancer. Loadbalancer takes in the optional parameter -v for debug mode.

Once the load balancer is running, run each of the frontend servers. These frontend servers will each connect to the load balancer, and occasionally send messages to the load balancer to confirm that they're still running. 

In order to connect to the frontend, a user should connect to localhost:8000. The load balancer will then automatically redirect the user to the relevant frontend server. It's imperative that all frontend servers are connected to the load balancer before any users attempt to connect. 

To launch backend servers: 
First, run all the storage node servers on port as per `./backend/config.txt` (storage node #1 should use the address and port specified in line 2, storage node #2 line 3 and so on). To run `backendserver`, type `./backendserver -v -p 20001 -i 1` for example, where -i is the index for the storage node. 

Then now start running `masterbackendserver` by typing `./masterbackendserver -d -v -p 20000 config.txt`. `masterbackendserver` will send periodic heart-beat messages to detect if any of the storage nodes is down. The communication between the frontend and backend must be initiated via a "INIT,rowkey" command sent to the backend master. The backend will then send back the address and port information of the storage node storing the data for the specified `rowkey` including the username/password login info.  
