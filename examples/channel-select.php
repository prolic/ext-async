<?php

namespace Concurrent;

$remaining = 0;

$producer = function (int $delay, Channel $channel) use (& $remaining) {
    try {
        $timer = new Timer($delay);
        $max = random_int(3, 5);

        for ($i = 0; $i < $max; $i++) {
            $timer->awaitTimeout();
            
            $channel->send($i);
        }
    } finally {
        $remaining--;
        
        $channel->close();
    }
};

$channel1 = new Channel();
$channel2 = new Channel();

$provider = new class($channel2) implements \IteratorAggregate {

    protected $channel;

    public function __construct(Channel $channel)
    {
        $this->channel = $channel;
    }

    public function getIterator()
    {
        return $this->channel->getIterator();
    }
};

function select(array $channels): \Generator
{
    while (true) {
        list ($k, $v) = Channel::select($channels);

        if ($k === null) {
            break;
        }

        yield $k => $v;
    }
}

$channels = [
    'A' => $channel1,
    'B' => $provider
];
$remaining = count($channels);

Task::async($producer, 40, $channel1);
Task::async($producer, 50, $channel2);

foreach (select($channels) as $k => $v) {
    var_dump($k, $v);
}
