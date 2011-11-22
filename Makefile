sup: sup.c
	gcc -Wall -lpthread sup.c -o sup
clean:
	rm sup
