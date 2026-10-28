package mineswitch.probe;

import java.awt.Insets;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.time.Duration;

/** Real standard API checks; no game, desktop window, or benchmark. */
public final class StandardApiProbe {
    private static int checks;
    private static void check(boolean value, String name) {
        if (!value) throw new IllegalStateException(name);
        ++checks;
        System.out.println("STANDARD_API_CHECK " + name + " PASS");
    }
    public static void main(String[] args) throws Exception {
        if (args.length > 1) throw new IllegalArgumentException("StandardApiProbe [OWNED_HTTP_FIXTURE_URL]");
        check(Runtime.version().feature() == 21, "java21");
        check(ModuleLayer.boot().findModule("java.net.http").isPresent(), "http_module_resolved");
        check(ModuleLayer.boot().findModule("jdk.jfr").isEmpty(), "jfr_stays_excluded");
        // Exercise the same data-class initializer selected by MidnightLib.
        // No AWT peer/window is created or claimed to be supported.
        System.setProperty("java.awt.headless", "false");
        Insets insets = new Insets(1, 2, 3, 4);
        check(insets.top == 1 && insets.left == 2 && insets.bottom == 3 && insets.right == 4,
              "genuine_insets_initializer");
        try (HttpClient client = HttpClient.newBuilder().version(HttpClient.Version.HTTP_1_1)
                .connectTimeout(Duration.ofSeconds(10)).build()) {
            check(client.followRedirects() == HttpClient.Redirect.NEVER, "real_http_client_builder");
            if (args.length == 1) {
                HttpRequest request = HttpRequest.newBuilder(URI.create(args[0]))
                    .timeout(Duration.ofSeconds(15)).GET().build();
                HttpResponse<String> response = client.send(request, HttpResponse.BodyHandlers.ofString());
                check(response.statusCode() == 200, "owned_http_fixture_status");
                check(response.body().equals("mineswitch-java21-http-ok\n"), "owned_http_fixture_body");
            }
        }
        System.out.println("STANDARD_API_PASS checks=" + checks);
    }
}
