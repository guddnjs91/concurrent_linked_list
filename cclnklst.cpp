#include <stdio.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

typedef unsigned long long mrkptr_t;

#define ISMARKED(p)		((unsigned long long)p & 0x8000000000000000)
#define MARKED(p)		((unsigned long long)p | 0x8000000000000000)
#define UNMARKED(p)		((unsigned long long)p & 0x7FFFFFFFFFFFFFFF)
#define REFERENCE(p)	UNMARKED(p)

typedef struct _Node
{
	int			key;

	mrkptr_t	next;

	_Node(int myKey)
	{
		key = myKey;
		next = NULL;
	}
} Node;

typedef struct _Window
{
	Node*		pred;
	Node*		curr;

	_Window(Node* myPred, Node* myCurr)
	{
		pred = myPred;
		curr = myCurr;
	}
} Window;

void init_list(Node** head, Node** tail)
{
	*head = new Node(INT_MIN);
	*tail = new Node(INT_MAX);

	(*head)->next = REFERENCE(*tail);
}

Window* find(Node* head, int key)
{
	Node* pred = NULL;
	Node* curr = NULL;
	Node* succ = NULL;
	bool marked = false;
	bool snip;
	bool retry;

	while( 1 ) // retry
	{
		pred = head;
		curr = (Node*)REFERENCE(pred->next);
		
		retry = false;
		while( 1 ) // iterate in the list
		{
			succ = (Node*)REFERENCE(curr->next);
			marked = ISMARKED(curr->next);
			while( marked )
			{
				// found marked node
				snip = __sync_bool_compare_and_swap( &pred->next,
													 curr,
													 succ );
				if( !snip )
				{
					// CAS failed
					retry = true;
					break;
				}
				curr = succ;
				succ = (Node*)REFERENCE(curr->next);
				marked = ISMARKED(curr->next);
			}
			if( retry )
				break;
			
			if( curr->key >= key )
				return new Window(pred, curr);
			
			pred = curr;
			curr = succ;
		}
	}
}

bool add(Node* head, int key)
{
	while( 1 )
	{
		Window* window = find(head, key);
		Node* pred = window->pred;
		Node* curr = window->curr;
		delete window;

		if( curr->key == key )
		{
			return false;
		}
		else
		{
			Node* node = new Node(key);
			node->next = UNMARKED(curr);
			if( __sync_bool_compare_and_swap( &pred->next,
											  curr,
											  node ) )
			{
				return true;
			}
		}
	}
}

bool remove(Node* head, int key)
{
	bool snip;

	while( 1 )
	{
		Window* window = find(head, key);
		Node* pred = window->pred;
		Node* curr = window->curr;
		delete window;

		if( curr->key != key )
		{
			return false;
		}
		else
		{
			Node* succ = (Node*)REFERENCE(curr->next);
			snip = __sync_bool_compare_and_swap( &curr->next,
												 succ,
												 MARKED(succ) );
			if( !snip )
				continue;
			__sync_bool_compare_and_swap( &pred->next,
										  curr,
										  succ );
			return true;
		}
	}
}

bool contains(Node* head, int key)
{
	bool marked = false;
	Node* curr = head;
	while( curr->key < key )
	{
		curr = (Node*)REFERENCE(curr->next);
		marked = ISMARKED(curr->next);
	}
	return ( curr->key == key && !marked );
}

#define TEST_CNT	10000
#define NUM_THREAD	24
Node* head;
Node* tail;

int thd_data[NUM_THREAD][TEST_CNT];

void* test_func(void* data)
{
	long tid = (long)data;

	int cnt_erase = 0;

	for( int i = 0; i < TEST_CNT; i++ )
	{
		thd_data[tid][i] = i * NUM_THREAD;
	}

	for( int i = 0; i < TEST_CNT; i++ )
	{
		int r = rand() % (TEST_CNT - i);
		
		int tmp = thd_data[tid][r];
		thd_data[tid][r] = thd_data[tid][TEST_CNT-i-1];
		thd_data[tid][TEST_CNT-i-1] = tmp;
	}

	for( int i = 0; i < TEST_CNT; i++ )
	{
		if( cnt_erase < i )
		{
			int r = rand() % 4;

			if( r == 0 )
			{
				remove( head, thd_data[tid][cnt_erase] + tid );
				cnt_erase++;
			}
		}

		add( head, thd_data[tid][i] + tid );
	}
	return (void*)cnt_erase;
}

int
main(void)
{
	srand(time(NULL));

	init_list( &head, &tail );

	pthread_t thd[NUM_THREAD];

	int cnt_erase[NUM_THREAD];

	// create threads
	for( long i = 0; i < NUM_THREAD; i++ )
	{
		if( pthread_create( &thd[i], NULL, test_func, (void*)i ) < 0 )
		{
			exit(0);
		}
	}

	// wait threads end
	long status;
	for( long i = 0; i < NUM_THREAD; i++ )
	{
		pthread_join( thd[i], (void**)&status );
		printf("%d, cnt_erase : %d\n", i, status);
		cnt_erase[i] = status;
	}

	// confirm consistency
	bool check[TEST_CNT * NUM_THREAD];
	int answer[TEST_CNT * NUM_THREAD];
	int cnt_answer = 0;

	for( int i = 0; i < TEST_CNT * NUM_THREAD; i++ )
	{
		check[i] = false;
	}
	for( int i = 0; i < TEST_CNT; i++ )
	{
		for( int j = 0; j < NUM_THREAD; j++ )
		{
			if( cnt_erase[j] > 0 )
			{
				cnt_erase[j]--;
			}
			else
			{
				check[ thd_data[j][i] + j ] = true;
			}
		}
	}
	for( int i = 0; i < TEST_CNT * NUM_THREAD; i++ )
	{
		if( check[i] )
		{
			answer[cnt_answer] = i;
			cnt_answer++;
		}
	}
/*
	for( int i = 0; i < cnt_answer; i++ )
	{
		printf("%d ", answer[i]);
	}
	printf("\n--------------------------\n");
*/
	int i = 0;
	Node* curr = (Node*)head->next;
	while( curr != tail )
	{
		if( i >= cnt_answer || curr->key != answer[i] )
		{
			printf("\nerror!!\n");
			break;
		}
		i++;
		curr = (Node*)curr->next;
	}
	printf("\n");

	return 0;
}

