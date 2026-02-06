#!/bin/sh

# 1. Check if mctpheartbeat is running
PID=$(pidof mctpheartbeat)

if [ -n "$PID" ]; then
    echo "Wrapper: Sending SIGTERM to mctpheartbeat (PID $PID)..."
    kill -TERM $PID
    
    # 2. Wait for the process to exit (Polling)
    #    The application has a built-in retry mechanism:
    #    - Max Retries: 5
    #    - Timeout per retry: 2.5s
    #    - Sleep between retries: 1s
    #    Total worst-case time: ~17.5s.
    #
    #    We wait up to 20s (200 * 0.1s) to allow all retries to complete if the SPI bus is busy.
    #    NOTE: This is a maximum timeout. If the app succeeds on the first try (normal case),
    #    it exits immediately, and this loop breaks instantly (no delay).
    count=0
    while [ -d "/proc/$PID" ] && [ $count -lt 200 ]; do
        sleep 0.1
        count=$((count+1))
    done

    if [ -d "/proc/$PID" ]; then
        echo "Wrapper: Timed out waiting for mctpheartbeat to exit. Proceeding with force reboot."
    else
        echo "Wrapper: mctpheartbeat exited gracefully."
    fi
fi

# 3. Execute the actual reboot
#    In OpenBMC/Systemd, the standard 'reboot' command is just an alias 
#    for 'systemctl reboot'. We call that directly here, passing all arguments 
#    (like -f) through to it.
exec /usr/bin/systemctl reboot "$@"
