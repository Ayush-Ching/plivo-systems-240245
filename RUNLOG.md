# Run Log

| Profile | Delay (ms) | Miss % | Overhead | Changes & Reasoning |
|---|---|---|---|---|
| A | 60 | > 1.00% | 1.00x | Baseline naive implementation. Failed due to lack of packet recovery. |
| A | 60 | 4.60% | 2.08x | Added K=3 XOR FEC and dynamic NACKs. Failed overhead cap (2.08x) due to math linear dependence on burst drops causing NACK spam. |
| A | 60 | 1.07% | 1.94x | Fixed FEC math to independent streams, but NACKs spammed at end of run due to hostile jitter cluster. |
| B | 100 | 1.20% | 1.98x | Decoupled K=2 FEC `(n-1 ^ n-3)` and 95% pacing, plus strict 25-packet NACK budget. Miss rate slightly above 1% cap for 100ms. |
| B | 110 | 0.93% | 1.99x | Same design, increased delay to 110ms. Passed! |
| B | 120 | 0.80% | 1.99x | Same design, increased delay to 120ms. Passed securely. |
