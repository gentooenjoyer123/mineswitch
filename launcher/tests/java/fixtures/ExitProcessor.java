package fixture;

/** Verifies that an ordinary early VM exit cannot publish runner success. */
public final class ExitProcessor {
    public static void main(String[] args) {
        System.out.print("fixture early exit without newline");
        System.exit(0);
    }
}
