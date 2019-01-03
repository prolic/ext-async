<?php

namespace Concurrent;

$producer = function (int $delay, Channel $channel) {
    try {
        $timer = new Timer($delay);
        $max = random_int(3, 5);

        for ($i = 0; $i < $max; $i++) {
            $timer->awaitTimeout();

            $channel->send($i);
        }
    } finally {
        $channel->close();
    }
};

$channel1 = new Channel();
$channel2 = new Channel();

$channels = [
    'A' => $channel1,
    'B' => $channel2->getIterator()
];

Task::async($producer, 80, $channel1);
Task::async($producer, 110, $channel2);

$timer = new Timer(200);
$block = ((int) ($_SERVER['argv'][1] ?? '0')) ? true : false;

$v = null;

for ($i = 0; $i < 20; $i++) {
    switch (Channel::select($v, $channels, $block)) {
        case 'A':
            var_dump('A >> ' . $v);
            break;
        case 'B':
            var_dump('B >> ' . $v);
            break;
        default:
            if ($block) {
                break 2;
            }
            
            printf("Await next poll...\n");
            $timer->awaitTimeout();
    }
}
