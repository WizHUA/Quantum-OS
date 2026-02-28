cmd_drivers/misc/quantum_os/modules.order := {   echo drivers/misc/quantum_os/quantum_os.ko; :; } | awk '!x[$$0]++' - > drivers/misc/quantum_os/modules.order
