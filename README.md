To launch frontend servers: 
First, run the loadbalancer. Loadbalancer takes in the optional parameter -v for debug mode.

Once the load balancer is running, run each of the frontend servers. These frontend servers will each connect to the load balancer, and occasionally send messages to the load balancer to confirm that they're still running. 

In order to connect to the frontend, a user should connect to localhost:8000. The load balancer will then automatically redirect the user to the relevant frontend server. It's imperative that all frontend servers are connected to the load balancer before any users attempt to connect. 