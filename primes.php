<?php
function main() {
    for ($i = 2; $i < 100000; $i++) {
        $is_prime = true;
        for ($divisor = 2; $divisor < $i; $divisor++) {
            if ($i % $divisor == 0) {
                $is_prime = false;
                break;
            }
        }
        if ($is_prime) {
        	echo "[$i]\n";
        }
    }
}
main();
