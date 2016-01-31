Refcounted C++ smart pointer that is atomically thread-safe, unlike Boost shared\_ptr which is not.

A couple of reference counted proxy collectors

And (eventually) a fast hazard pointer implementation (no store/load memory barrier)