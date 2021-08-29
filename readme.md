This is a very basic modbus proxy beetween smartevse controller and smartbox v2.

The problem is mikrotik knot's are not capable of transparently connect modbus master device to modbus slaves.
So instead I pretend that my master is a slave device.

Master device only sends one fixed request to slave, so this proxy generates it instead.
And then listens to response and forwards it to master device it is.

Hopefully in the future knot modbus support will be sufficient to do it directly.

In meanwhile:

1) compile it with gcc main.c -o main
2) put it in "/home/pi/evseproxy/main"
3) add /etc/systemd/system/smartevse.service

	[Unit]
	Description=SmartEVSE proxy daemon
	After=network.target

	[Service]
	ExecStart=/home/pi/evseproxy/main

	[Install]
	WantedBy=multi-user.target

4) run

	sudo systemctl start smartevse
	sudo systemctl enable smartevse