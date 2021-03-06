#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile with
 * gcc -g zoomx.c -L/usr/X11R6/lib -lX11 -o zoomx
 * ./zoomx
 */

XImage* ScaleXImage(XImage* originalImage, double scale, Display* display, Visual* visual, int depth)
{
   double scaleWidth = scale;
   double scaleHeight = scale;
   
   double w2 = originalImage->width * scale;
   double h2 = originalImage->height * scale;

   /* Allocate space for scaled image data */
   char *dataz = malloc(w2 * h2 * 4);
   memset(dataz, 0, w2 * h2 * 4);

   /* Copy existing image to modify */
   long pixel = 0;
   XImage* scaledImage = XCreateImage(display, visual, depth, ZPixmap, 0, dataz, w2, h2, 8, 0);

   for (int x = 0; x < scaledImage->width; x++)
   {
      for (int y = 0; y < scaledImage->height; y++)
      {
         /* Figure out the closest pixel on the original image */
         int x1 = (int)(x / scaleWidth);
         int y1 = (int)(y / scaleHeight);
         pixel = XGetPixel(originalImage, x1, y1);

         /* Put this pixel onto the scaled image */
         XPutPixel(scaledImage, x, y, pixel);
      }
   }

   return scaledImage;
}

XEvent CreateFullscreenRequest(Display* display, Window window)
{
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);

    XEvent e;
    memset(&e, 0, sizeof(e)); /* ensure XEvent is zeroed */
    e.type = ClientMessage;
    e.xclient.window = window;
    e.xclient.message_type = wm_state;
    e.xclient.format = 32;
    e.xclient.data.l[0] = 1;
    e.xclient.data.l[1] = fullscreen;
    e.xclient.data.l[2] = 0;
    return e;
}

struct ViewLocation
{
   int Top;
   int Left;
};

struct ViewLocation GetMouseLocation(Display* display, Window window)
{
   Window rootWindow;
   Window childWindow;
   int rootX;
   int rootY;
   int childX;
   int childY;
   unsigned int mask;
   Bool pointerOnWindow = XQueryPointer(display, window, &rootWindow, &childWindow, &rootX, &rootY, &childX, &childY, &mask);

   struct ViewLocation viewLocation = { .Top = rootY, .Left = rootX };
   return viewLocation;
};

void CenterView(Display* display, Window window, int screenWidth, int screenHeight, double sf, double lastSf, Bool centerOnMouse, struct ViewLocation* viewLocation)
{
   /* The target center of the image is either the current mouse location or the center of the screen */
   struct ViewLocation mouse;
   if (centerOnMouse)
   {
      mouse = GetMouseLocation(display, window);
   }
   else
   {
      mouse.Left = 0.5 * screenWidth;
      mouse.Top = 0.5 * screenHeight;
   }

   /* Delta scale */
   double dsf = sf / lastSf;

   /* Adjust for scaling */
   viewLocation->Left *= dsf;
   viewLocation->Top *= dsf;

   /* Find the desired center of the image, in pixels */
   int centerX = viewLocation->Left + mouse.Left * dsf;
   int centerY = viewLocation->Top + mouse.Top * dsf;

   /* Account for screen size to determine the corresponding top left corner */
   int cornerX = centerX - 0.5 * screenWidth;
   int cornerY = centerY - 0.5 * screenHeight;

   /* Set the new view location */
   viewLocation->Left = cornerX;
   viewLocation->Top = cornerY;  
}

void PutXImageWithinBounds(Display* display, Window window, GC graphicsContext, XImage* image, struct ViewLocation* viewLocation)
{
   /* Get the geometry of the window that is currently being displayed to. */
   Window rootWindow;
   int windowX;
   int windowY;
   unsigned int windowWidth;
   unsigned int windowHeight;
   unsigned int windowBorder;
   unsigned int windowDepth;
   XGetGeometry(display, window, &rootWindow, &windowX, &windowY, &windowWidth, &windowHeight, &windowBorder, &windowDepth);

   /* Too far to the left if the right side of the image is encroaching on the edge of the window. 
    * Positive means the right side of the image is safely off to the right and out of sight.
    * Negtive means the right side of the image is on window, so we'll correct for that. */
   int rightEncroachment = (image->width - viewLocation->Left) - windowWidth;
   if (rightEncroachment < 0)
      viewLocation->Left += rightEncroachment;

   /* Too far to the bottom is a similar story */
   int bottomEncroachment = (image->height - viewLocation->Top) - windowHeight;
   if (bottomEncroachment < 0)
      viewLocation->Top += bottomEncroachment;

   /* Cannot place the top of the image above the top of the window */
   if (viewLocation->Top < 0)
      viewLocation->Top = 0;

   /* Cannot place the left of the image to the left of the window */
   if (viewLocation->Left < 0)
      viewLocation->Left = 0;

   XPutImage(display, window, graphicsContext, image, viewLocation->Left, viewLocation->Top, 0, 0, image->width, image->height);
}

int main(void)
{
   /* Open the X display. If this doesn't work, we can't do anything */
   Display* display = XOpenDisplay(NULL);
   if (display == NULL)
   {
      fprintf(stderr, "Cannot open X display\n");
      exit(1);
   }

   /* Read settings (or command line arguments...?) */
   Bool startFullscreen = True;
   Bool centerOnMouse = False;
   double defaultScaleFactor = 2.0;
   double maxScaleFactor = 5.0;
   double scaleFactorIncrement = 1.0;
   int panIncrement = 100;

   /* Get the screen and compute its dimensions */
   int screen = DefaultScreen(display);
   int screenWidth = DisplayWidth(display, screen);
   int screenHeight = DisplayHeight(display, screen);

   /* Get some default visual items */
   Window rootWindow = RootWindow(display, screen);
   GC graphicsContext = DefaultGC(display, screen);
   Visual* visual = DefaultVisual(display, screen);
   int depth = DefaultDepth(display, screen);

   /* Get an image of the current the full screen. */
   XImage* screenshot = NULL;
   screenshot = XGetImage(display, rootWindow, 0, 0, screenWidth, screenHeight, AllPlanes, ZPixmap);

   /* Scale the image by two for initial display */
   double currentScaleFactor = defaultScaleFactor;
   XImage* scaledImage = NULL;
   scaledImage = ScaleXImage(screenshot, currentScaleFactor, display, visual, depth);

   /* Create the window to look at and subscribe to events */
   long blackPixel = BlackPixel(display, screen);
   long whitePixel = WhitePixel(display, screen);
   Window window = XCreateSimpleWindow(display, rootWindow, 10, 10, 100, 100, 1, blackPixel, whitePixel);
   XSelectInput(display, window, ExposureMask | KeyPressMask);
   XMapWindow(display, window);

   /* Figure out where the image should be panned to */
   struct ViewLocation viewLocation = { .Left = 0, .Top = 0 };
   CenterView(display, window, screenWidth, screenHeight, currentScaleFactor, 1.0, centerOnMouse, &viewLocation);

   /* Show window and make fullscreen if requested */
   if (startFullscreen)
   {
      XEvent fullscreenRequest = CreateFullscreenRequest(display, window);
      XSendEvent(display, rootWindow, False, SubstructureRedirectMask | SubstructureNotifyMask, &fullscreenRequest);
   }

   /* Event loop */
   XEvent e;
   while (1)
   {
      XNextEvent(display, &e);

      /* Window was exposed, so draw it! 
       * In this case we just draw the image onto the window. */
      if (e.type == Expose)
      {
         PutXImageWithinBounds(display, window, graphicsContext, scaledImage, &viewLocation);
      }

      if (e.type == KeyPress)
      {
         /* Get a comparable symbol for the pressed key. */
         char buf[128] = {0};
         KeySym keysym;
         int len = XLookupString(&e.xkey, buf, sizeof buf, &keysym, NULL);

         /* Modify panning increment based on modifer keys */
         int modifiedPanIncrement = panIncrement;
         if (e.xkey.state & ShiftMask)
            modifiedPanIncrement *= 4;

         /* Exit program for Escape key or q key*/
         if (keysym == XK_Escape || keysym == XK_q)
            break;

         /* Check for zoom in */
         else if (keysym == XK_plus || keysym == XK_equal || keysym == XK_Page_Up)
         {
            int lastScaleFactor = currentScaleFactor;
            currentScaleFactor += scaleFactorIncrement;
            if (currentScaleFactor > maxScaleFactor)
               currentScaleFactor = maxScaleFactor;

            if (currentScaleFactor != lastScaleFactor)
            {
               XDestroyImage(scaledImage);
               scaledImage = NULL;
               scaledImage = ScaleXImage(screenshot, currentScaleFactor, display, visual, depth);
               CenterView(display, window, screenWidth, screenHeight, currentScaleFactor, lastScaleFactor, centerOnMouse, &viewLocation);
               PutXImageWithinBounds(display, window, graphicsContext, scaledImage, &viewLocation);
            }
         }

         /* Check for zoom out. Scale cannot be less than 1.0 */
         else if (keysym == XK_minus || keysym == XK_Page_Down)
         {
            int lastScaleFactor = currentScaleFactor;
            currentScaleFactor -= scaleFactorIncrement;
            if (currentScaleFactor < 1.0)
               currentScaleFactor = 1.0;

            if (currentScaleFactor != lastScaleFactor)
            {
               XDestroyImage(scaledImage);
               scaledImage = NULL;
               scaledImage = ScaleXImage(screenshot, currentScaleFactor, display, visual, depth);
               CenterView(display, window, screenWidth, screenHeight, currentScaleFactor, lastScaleFactor, centerOnMouse, &viewLocation);
               PutXImageWithinBounds(display, window, graphicsContext, scaledImage, &viewLocation);
            }
         }

         /* Move view with arrow keys or WASD keys */
         else if (keysym == XK_Right || keysym == XK_d || keysym == XK_D)
         {
            viewLocation.Left += modifiedPanIncrement;
            PutXImageWithinBounds(display, window, graphicsContext, scaledImage, &viewLocation);
         }
         else if (keysym == XK_Left || keysym == XK_a || keysym == XK_A)
         {
            viewLocation.Left -= modifiedPanIncrement;
            PutXImageWithinBounds(display, window, graphicsContext, scaledImage, &viewLocation);
         }
         else if (keysym == XK_Up || keysym == XK_w || keysym == XK_W)
         {
            viewLocation.Top -= modifiedPanIncrement;
            PutXImageWithinBounds(display, window, graphicsContext, scaledImage, &viewLocation);
         }
         else if (keysym == XK_Down || keysym == XK_s || keysym == XK_S)
         {
            viewLocation.Top += modifiedPanIncrement;
            PutXImageWithinBounds(display, window, graphicsContext, scaledImage, &viewLocation);
         }
      }
   }

   /* Clean up and exit */
   if (screenshot != NULL)
   {
      XDestroyImage(screenshot);
      screenshot = NULL;
   }

   if (scaledImage != NULL)
   {
      XDestroyImage(scaledImage);
      scaledImage = NULL;
   }

   XCloseDisplay(display);
   display = NULL;
   return 0;
}