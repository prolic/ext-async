<?php

namespace Concurrent;

$work = function ($label, iterable $it) {
    try {
        foreach ($it as $v) {
            printf("%s -> %s\n", $label, $v);
        }
    } catch (ChannelClosedException $e) {
        printf("[%s] %s\n", get_class($e), $e->getMessage());
        printf("-> [%s] %s\n", get_class($e->getPrevious()), $e->getPrevious()->getMessage());
    } catch (\Throwable $e) {
        echo $e, "\n\n";
    }
};

$channel = new Channel();

Task::async($work, 'A', $channel);
Task::async($work, 'B', $channel->getIterator());

$timer = new Timer(20);

for ($i = 0; $i <= 20; $i++) {
    $timer->awaitTimeout();
    $channel->send($i);
}

$channel->close();
