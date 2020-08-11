# foo_out_scream
This is a(n experimental) foobar2000 component for streaming over network directly using the [Scream](https://github.com/duncanthrax/scream) protocol. Only supports multicast and only tested using the PulseAudio receiver.

It is certainly less reliable than UPnP MediaRenderer Output, but visualisations work. Visualisations will be out of sync due to latency on the receiver, which we cannot measure, but by default for the PulseAudio receiver this is only 50ms.
