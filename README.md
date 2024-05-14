# Names: Anna Xia, Andrew Jiang, Joseph Himes, Sean Chuang

# How to make project:

`cd /frontend`

`apt-get install uuid-dev` to install uuid package

`make`

`cd ../backend`

`make`

# To launch frontend servers: 
First, run the loadbalancer in the frontend directory `./loadbalancer`. Loadbalancer takes in the optional parameter -v for debug mode.

Once the load balancer is running, run each of the frontend servers. 

`./frontendserver -p 8080`

`./frontendserver -p 8081`

`./frontendserver -p 8082`

These frontend servers will each connect to the load balancer, and occasionally send messages to the load balancer to confirm that they're still running. 

In order to connect to the frontend, a user should connect to localhost:8000. The load balancer will then automatically redirect the user to the relevant frontend server. It's imperative that all frontend servers are connected to the load balancer before any users attempt to connect. 

# To launch backend servers: 
First, the working directory needs to be `root/backend/` (type `cd ./../backend/` if coming from the previous step). Compile all the source code by typing `make`. 

Now, open 9 Terminal tabs. 
In the 1st tab, run `./backendserver -c config.txt -i 1`.

In the 2nd tab, run `./backendserver -c config.txt -i 2`.

In the 3rd tab, run `./backendserver -c config.txt -i 3`.

In the 4th tab, run `./backendserver -c config.txt -i 4`.

In the 5th tab, run `./backendserver -c config.txt -i 5`.

In the 6th tab, run `./backendserver -c config.txt -i 6`.

In the 7th tab, run `./backendserver -c config.txt -i 7`.

In the 8th tab, run `./backendserver -c config.txt -i 8`.

In the 9th tab, run `./backendserver -c config.txt -i 9`.

Here, `-i`'s argument refers to the storage node numberm and `-c`'s the config file's path. 

Now open another tab. 
In the tab, run `./masterbackendserver -d -v config.txt`. 

To clear the memory on disk for storage node #1, navigate to backend/storage_node_1/activity_logs and remove each "tablet_log_" .txt file. Next, navigate to backend/storage_node_1/tablets and remove each "tablet_" .txt file. This information is also stored in a README file in each directory.

# To launch SMTP server/client:
`./smtpserver -v` runs the server to receive emails from outside penn cloud system but still running on same host machine (ie Thunderbird)

`./smtpclient -v` runs the client to send emails to external domains like (@seas.upenn.edu or @gmail.com)
