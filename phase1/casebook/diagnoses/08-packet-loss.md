# 08-packet-loss

**Label:** `packet_loss`

## Root cause
Random loss on the path (bad link, Wi-Fi, congested queue). TCP keeps working but slowly.

## Expected signature in the mini-tcpdump summary
- Many server→client `retransmissions` and client `dup_acks`, spread across the transfer
- Transfer *completes*: `bytes_acked` large, flow `closed`; that separates loss from a black hole
- RTT samples look normal; it's the retransmissions that cost time

## Real-world fix
Find the lossy hop (mtr, per-interface error counters), fix the link or the queue. Not fixable at the app layer.

## Observed
<!-- Fill in after `sudo ./run_case.sh 08-packet-loss`: paste the relevant lines from
`mini-tcpdump -r pcaps/08-packet-loss/run1.pcap -q` and note anything that differed from the expected signature. -->
