/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * Provides infrastructure for automated download components tests.
 */

"use strict";

////////////////////////////////////////////////////////////////////////////////
//// Globals

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cu = Components.utils;
const Cr = Components.results;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "Downloads",
                                  "resource://gre/modules/Downloads.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "FileUtils",
                                  "resource://gre/modules/FileUtils.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "HttpServer",
                                  "resource://testing-common/httpd.js");
XPCOMUtils.defineLazyModuleGetter(this, "NetUtil",
                                  "resource://gre/modules/NetUtil.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "Promise",
                                  "resource://gre/modules/commonjs/sdk/core/promise.js");
XPCOMUtils.defineLazyModuleGetter(this, "Services",
                                  "resource://gre/modules/Services.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "Task",
                                  "resource://gre/modules/Task.jsm");

const ServerSocket = Components.Constructor(
                                "@mozilla.org/network/server-socket;1",
                                "nsIServerSocket",
                                "init");

const HTTP_SERVER_PORT = 4444;
const HTTP_BASE = "http://localhost:" + HTTP_SERVER_PORT;

const FAKE_SERVER_PORT = 4445;
const FAKE_BASE = "http://localhost:" + FAKE_SERVER_PORT;

const TEST_SOURCE_URI = NetUtil.newURI(HTTP_BASE + "/source.txt");
const TEST_FAKE_SOURCE_URI = NetUtil.newURI(FAKE_BASE + "/source.txt");

const TEST_INTERRUPTIBLE_PATH = "/interruptible.txt";
const TEST_INTERRUPTIBLE_URI = NetUtil.newURI(HTTP_BASE +
                                              TEST_INTERRUPTIBLE_PATH);

const TEST_TARGET_FILE_NAME = "test-download.txt";
const TEST_DATA_SHORT = "This test string is downloaded.";

/**
 * All the tests are implemented with add_task, this starts them automatically.
 */
function run_test()
{
  run_next_test();
}

////////////////////////////////////////////////////////////////////////////////
//// Support functions

/**
 * Returns a reference to a temporary file.  The file is deleted if it already
 * exists.  If the file is then created by the test suite, it will be removed
 * when tests in this file finish.
 */
function getTempFile(aLeafName)
{
  let file = FileUtils.getFile("TmpD", [aLeafName]);
  function GTF_removeFile()
  {
    if (file.exists()) {
      file.remove(false);
    }
  }

  // Remove the file in case a previous test created it.
  GTF_removeFile();

  // Remove the file at the end of the test suite.
  do_register_cleanup(GTF_removeFile);

  return file;
}

/**
 * Ensures that the given file contents are equal to the given string.
 *
 * @param aFile
 *        nsIFile whose contents should be verified.
 * @param aExpectedContents
 *        String containing the octets that are expected in the file.
 *
 * @return {Promise}
 * @resolves When the operation completes.
 * @rejects Never.
 */
function promiseVerifyContents(aFile, aExpectedContents)
{
  let deferred = Promise.defer();
  NetUtil.asyncFetch(aFile, function(aInputStream, aStatus) {
    do_check_true(Components.isSuccessCode(aStatus));
    let contents = NetUtil.readInputStreamToString(aInputStream,
                                                   aInputStream.available());
    if (contents.length <= TEST_DATA_SHORT.length * 2) {
      do_check_eq(contents, aExpectedContents);
    } else {
      // Do not print the entire content string to the test log.
      do_check_eq(contents.length, aExpectedContents.length);
      do_check_true(contents == aExpectedContents);
    }
    deferred.resolve();
  });
  return deferred.promise;
}

/**
 * Starts a socket listener that closes each incoming connection.
 *
 * @returns nsIServerSocket that listens for connections.  Call its "close"
 *          method to stop listening and free the server port.
 */
function startFakeServer()
{
  let serverSocket = new ServerSocket(FAKE_SERVER_PORT, true, -1);
  serverSocket.asyncListen({
    onSocketAccepted: function (aServ, aTransport) {
      aTransport.close(Cr.NS_BINDING_ABORTED);
    },
    onStopListening: function () { },
  });
  return serverSocket;
}

/**
 * This function allows testing events or actions that need to happen in the
 * middle of a download.
 *
 * Calling this function registers a new request handler in the internal HTTP
 * server, accessible at the TEST_INTERRUPTIBLE_URI address.  The HTTP handler
 * returns the TEST_DATA_SHORT text, then waits until the "resolve" method is
 * called on the object returned by the function.  At this point, the handler
 * sends the TEST_DATA_SHORT text again to complete the response.
 *
 * You can also call the "reject" method on the returned object to interrupt the
 * response midway.  Because of how the network layer is implemented, this does
 * not cause the socket to return an error.
 *
 * The handler is unregistered when the response finishes or is interrupted.
 *
 * @returns Deferred object used to control the response.
 */
function startInterruptibleResponseHandler()
{
  let deferResponse = Promise.defer();
  gHttpServer.registerPathHandler(TEST_INTERRUPTIBLE_PATH,
    function (aRequest, aResponse)
    {
      aResponse.processAsync();
      aResponse.setHeader("Content-Type", "text/plain", false);
      aResponse.setHeader("Content-Length", "" + (TEST_DATA_SHORT.length * 2),
                          false);
      aResponse.write(TEST_DATA_SHORT);

      deferResponse.promise.then(function SIRH_onSuccess() {
        aResponse.write(TEST_DATA_SHORT);
        aResponse.finish();
        gHttpServer.registerPathHandler(TEST_INTERRUPTIBLE_PATH, null);
      }, function SIRH_onFailure() {
        aResponse.abort();
        gHttpServer.registerPathHandler(TEST_INTERRUPTIBLE_PATH, null);
      });
    });
  return deferResponse;
}

////////////////////////////////////////////////////////////////////////////////
//// Initialization functions common to all tests

let gHttpServer;

add_task(function test_common_initialize()
{
  // Start the HTTP server.
  gHttpServer = new HttpServer();
  gHttpServer.registerDirectory("/", do_get_file("../data"));
  gHttpServer.start(HTTP_SERVER_PORT);
});
