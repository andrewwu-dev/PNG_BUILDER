
# PNG Builder
A concurrency lab to use threads to download multiple images and concatenate them into one image using the producer-consumer approach. [PNG Structure](https://www.oreilly.com/library/view/png-the-definitive/9781565925427/17_chapter-08.html). A server provides several pieces of a full image and the program will attempt to add the IDAT data of each of those pieces together inorder to reproduce the full image. The main file is paster2.c

## Process
We'll concatenate images by adding PNG IDAT data received of each image downloaded from the server into one array and then using this info to write a .PNG file as output.
1. Initialize shared memory for semaphores, a variable that stores image pieces, and one that stores overall IDAT data
2. Create *P* threads that download one piece of the image from an external server and store it in image pieces array
3. Create *C* threads that remove items from image pieces array and adds the IDAT info of each piece to the overall IDAT array
4. Once all images are downloaded and added to the overall IDAT array, write to a new file with the IDAT plus the IHDR and IEND info.
