import java.lang.reflect.InvocationTargetException;
import java.util.Arrays;

/** Host-only adapter: explicitly registers the candidate on the boot SHA5 class. */
public final class Sha5HostRunner {
    private static native long calls();

    public static void main(String[] args) throws Exception {
        if (args.length < 2) throw new IllegalArgumentException("native-library probe-class [args]");
        System.load(args[0]);
        try {
            Class.forName(args[1]).getMethod("main", String[].class)
                    .invoke(null, (Object) Arrays.copyOfRange(args, 2, args.length));
        } catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof Exception exception) throw exception;
            if (failure.getCause() instanceof Error error) throw error;
            throw failure;
        }
        long count = calls();
        if (count == 0) throw new AssertionError("Native SHA5 candidate was not exercised");
        System.out.println("SHA5_HOST_NATIVE_CALLS=" + count);
    }
}
