import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.security.CodeSigner;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.jar.JarEntry;
import java.util.jar.JarFile;

/** Public-JCA regression tests shared by the host candidate and real device. */
public final class HorizonSha5Probe {
    private static int checks;
    private static final String[] ALGORITHMS = {"SHA-384", "SHA-512", "SHA-512/224", "SHA-512/256"};
    private static final String[] ABC = {
        "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        "4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa",
        "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23"
    };
    private static final int[] LENGTHS = {0, 1, 2, 3, 31, 63, 64, 110, 111, 112, 113, 127, 128, 129, 191, 239, 240, 255, 256, 257, 511, 1023, 1024, 1025, 8191, 8192, 8193, 16383, 16384, 16385, 32769, 100003};
    private static final String[][] EXPECTED = {
        {
            "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b",
            "2deb5d512a82fb61b846799f41367b0172a8a82a4b1539fa9f3fd148bdd1db7eb053b452eb916523186cbbe41e9f2fbb",
            "adae964ee62b722cfea088ac283bc1c9446df54d5d42121c047c8979f26f9c462ca8c27cad5eede3e9226cf7fec63090",
            "da78ef0d5c2d8b8c8cfb5331799cf5d82fdb7c797031b8a808f2620ae844083c2cf3cbc1146d56b7cbca8aaaa0adffc0",
            "ba5d7d321c953b2cb319b55b43027a85e577093f0a8fb1db3f840161800024e746c90ad12c0b07d9fc5493833686a963",
            "5a9b4fa6a587f9551d9a2e6b9f372eea00c20fff4a7c15e57841caf571355a2c436563f174f5889bbf6dde0ab4f0169c",
            "509268a6356636a9e69ed54749b469a271d9ff994158946759cf174bc448f0e602fa170419111cc9dd23f73d978f1958",
            "bb2e846f2aa567452ed08dd54015e56599167a041e061897877f80dca341f627f9602ec7d2481a7f96471d3a790f2786",
            "69f6e36aa293dc10a5e9e267546b12ab1c176b7fbfacde331f151cfaf0c74da80931ae1cd52d28f7413063523726d3f7",
            "c2c1bef7ae4c2865fc3aaac17286ddc8077185a9fe3400627a6d9257f46a8d86485e2d92af0d18d43b3336c503505e59",
            "90134d980ed351a7f6a98bebcf685981db55b44541c3b2cb40a82a635b216d0dc22251aac46f5d56e7d7bc5d802034a4",
            "929432a0755d5c4e12c13794c1eeabf6382cfd0b40dd1a14d00283a1174c453422defac18fa5e5c2519c00044f890c1b",
            "10587c5fb24a0155159e88d64132e4d3abe3f857102bd6f93b508410a9865b8808156145f30f37e0b91e7d800995a2d9",
            "54f1b21b641b73e25e019341eb763026f13292fc9c4ddbf8f9945bb6d0c27888fe013388fb2c4cd6a2db8a3181fdde4f",
            "6ef82a5100af6c3ed45188505148a760602fcb895e35adfd639c553615240fad28ffba4e7f01f1605aa15ae3d14e0a85",
            "b5d13d9046f7b09d1b0838ac1b8482fc1c24c1149c95a21d2b2250861e16d8a46d76a2efec0d5735d9be5466b12bb23a",
            "f871c1d10b515a94039ad91c23a568dc1f586ffe2f7d24376a006c958c5e437cfc367c69e0a0459be457cafc09afc76f",
            "5b5eb853a75c43718c66429e80d95ddd157caf43497295e22227feb91f8616a93f34b7a271eca3cb0c34c7d215ac3f28",
            "c89e6d76ad1e4ab1a5af97e854160caf842114b0fcdb13e9e2d65583a04bdf4db0d9da4884d3a6101cd5fced88470944",
            "926b54ba8da1320f1f6f8b3d95babcd220155d0f537db02df23d560610dae919483d05e10cac889a5698b9df9d3a9f6e",
            "79c520e163898856caf705110ca4f9cb32dce0e9666398e0d50ba94bcef582124f508744d235a51af76581c58a0b3c86",
            "d1bd8810fb021b6c906f0332830443d256e466fc8d6138933f5e7d3eeacdc62cbb3d4d9144ffb991626cc2344e3b8025",
            "d778e0b116f43f0f2ec004d159d55cc4cea590f1909b159c9915da7127c0040e7949d270a6fac407ec0e9105c7c3aeae",
            "55c787c124eb29413f0e3add1967adabc830f57e56329dff9207fa2743d1556be9efa4445cdd1617943121a43a26f325",
            "4382ac78e1654b4fd1328bcc0fbd223c81a59dbe3c29ca6907ad5307a45003aa364b180a89c148b2f119012f7525b574",
            "28a28765443daef2ccaa3ba96ea68e31ee5514b0e6dba187494b15bc17e38aeec8c7e1eafc13b3c5e0f358fcb462e071",
            "50b53bcc982af539120a903f057a27b2ce9f5f420b96c6f306da1d51e96a4e9c9b8e779dbf835090e898c8fcaacbae76",
            "d88d0e9ea6d9d97a00dedbbee26245d0e7cd85b8ab8927a436398d517ef21108207aa365d589a04a7ad83bdb27898ade",
            "597ff66b1b9b7e49627b856afc568d3d7494159081be8f672bfbf73a7198eead6672d04bae7a4e1821c9f25d159b5b77",
            "6234e56c4dbb841a6c27efdc3167f23f0b9859209e3719b6877320a043d42d6d0f0baad0c86ed7ddd2c96538ba383057",
            "ed9d4473c2a774c702d0adfb62b6688845ac535b7b2cbb9527f96c3f3cb291e5ac840afc02c81170bd995e04608aa018",
            "5c5def4040771af383d771938d8432a778bd39a7acb4bf03cef179e28bb562e13c50cebd819e7af836b4a9bff6124f69"
        },
        {
            "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
            "527cff2b6fdfbc0f54fe092b17d6d8c7e22500242635fa56981e85a64da6ce8a12a3a66cf69fd48f588bcba9bad141b8e351a0cdd4925ae57289933eec1fc153",
            "32c25b09894d0062567a0fec87fd7ec00a7a02d4d03480c2b1f09e320d4ac89b68f91925983a155d0c20b20e90f4307d4004b2427e3d248a3ff8f22c6deea3e1",
            "b634899da10b86dd03ad5e7cef4279fa2addbc82a4fdc782c99d95d776ceec8777201d4acdf259e9f935fa43ff71ac7e7317220734b9f43bd9192c8bdf46ac8c",
            "fee9c576c4d8dcdc8163f489682053a6c5a4653f3274170e752895490233c0aaa252e1b17910a0812b9abff58d86737d4354bc58680b97f6e9e9db43b6edacf0",
            "b632d37ec2382175dffe0f45338efb5c27a47b999915037d4a9b39d6ed16c3f83d5248bd116e060b435edc3a7df35432252462db3f77d6333e0c98e26b150605",
            "5fc82c8b604168299ef67c01b373a52673a31839931d4473185f864f97304225c0af6a3554ff2d027fb5f12f7ad19aa681f091eab032833cd99363516376293f",
            "6f31ec446bcc49dece43f6622708b2a150cda30b309a7a8dc539c7c50f943c25d0d3f128e2a0f8e3382fc2fa62d0843ac06de1e5e6e1360969524ee94965d58c",
            "25cc17ea72e6283144c30a63edb2700a58bc1c60a1cf2bd08162e4c5027434c97f6b1fefd5cc24082cdc5cd7c3db4e17724facc759bb735b64552cc2bb661c6f",
            "38c9a7acd9fdb457ec823c1dc80c43d5b1144d76a7c4d2ec678b250ec700bca66531f7f124875258fc9c28fbdf2fcd2e475ba6d9b1abf883b99c424120621ccf",
            "695d3c119052891e183076fa72fa7cdcb797d19c8ed03a168742556ecfbb623e7a8f68ba5eac30b8de8187cff7207ef9cf351b6949e808109bdacc9c4c5ccc8e",
            "feec7dd990a97f606f05d3ac2a12d07f3eebddcbf756efebf8d103873faa6a9f4c424104139c7a2fb3cd8aeb4fb529336eccef5f59b0174f8eb1cffd6e229c22",
            "eb2b35a356e38b09c7dff47b0408ad89968c11165fec57768d169620872bc240ce4e592f78b64a3a557b5341c8e3fb191491aae0494079607a43ca17df204aae",
            "b2e4a59edcd4ecea1fbb8b2c574a0eb773fa244fcc9f7198a6e6531a02b1da846c1cbb66b201dc7e11b39353e3b18c476cedadb3fa58c2ae65605f33a4a485cd",
            "cba2f1a9dec960e9dc519d7701b89233d94329462edd6af036c752ec965ad425b02c265d9cdaea5a0d4100fb4c58c1d995b91222bd97daaeca4d08aa9ada1620",
            "c4ceaab91375253574843456d39ae4250c4b470beeb375b0228e56ba505bd5e827403f5461219950ef686d807421b3bb1fa6e336b3d823a1151adc5dd44ce79e",
            "a323598ef1c49191b723c60abbae0478c4220150ac3993505f3da7bb9ad40b3f136a23baf1e9a492cbf3da4827c2c453faaa379b200077364e3df75a11e21484",
            "3212b876107738591dc91413986b05a15612f507e34cbbab4beba1a7b34f01c6425e63f0d4a7429295e59fb2929c65e707a7e7c9124f28fb45ad39808f03302b",
            "57b376c2fc436d5544685ef7cec3d29250a92221ed7d0f4b84d4ff2676129d0961f122812f0d643c12862be1ed07e012c827bc7c47a707d542badc5d59ef7a8e",
            "f54f559c8faf1073a8b1255c0aab651e6d4677085409faa9ce0e81c0c9b788271fd243e683bbc8140145ea3d5f15a63e94afcfc646a6fec2f6e199e71a2d3be0",
            "0467d238452e8cdb9e4c2f4a21af964941a7a23b6b4f959a8d6228f3c9cfa147b5d8ac1dc9cedec1a160cda9f3e43711ebc483718af15886b1dbc86ce0737e06",
            "5e45da334d36a7315466f3fc1c7ca98505125be85c2b641063c5db6c306eab5f64d8bee71e3f9720fb18d616fed7d0ebc1cc389d5030758f0c25f2ed90c32df3",
            "251daa872d2c0c412ed11a4d7b2a0c4727548bdea67d90cb15258471a3aa644a190b3d4cc462de37979840598d76ff9db313e7fcceab6995895abf7a27398a08",
            "eef17459fe8652a78938932953b0805a00e3855968d2f033bd44d429ac538a35686815ca3a357777d08af8540c4ce701d79eea1e4eb3721e0e93b727fbdcbb68",
            "0a7f5ade30b7e1ddf05c0e189b9441164a388969e16f8fcb2ce487d414f69fcf1e7565d092c8cc776b385feb70b68096e3acccdd643cf98fb58cfef3f923759e",
            "90ebb9de24ba08eb250cc35b56c5af305a4605a841a385902ad38c11cb9bcb38803294f68e8061136880b248cd0e7ac3ca023ed7d49d7bef86ccfb2e08aee8aa",
            "7f331bd4645dd3010c41e76a160e8a61ae85a169b06afe9e45933e3ee10baad2a7501814ab2d667b31b7c1121295248b201e0524ba1e64849cf66ea100fcc0c5",
            "81f536a2f4fbd59854fcb3c49f2223566b1c0fa22ebc5a927b1789219118e78157697a6ccbf366e97776a885c68258c5c89a86a1481db72394a249a1867c874d",
            "22e01f72ad0626fb50e296e2bf3faafd93636108db0e864c4588f2010f36f68d8baf1278883c1c6d0f08c90097a1532e9c3df076d3399b6bf68e07af44a4425c",
            "1226752615b1ce5d2d518571b33d56bd6a3850c116f42eb3fe18fc12645687656be57a0df5e8fbbfe91f5a63ca68a15463591d046dbe1256b6e8b24d8bb705cc",
            "7120f05f53ec2f79a82888237da4913f8811bab1d0443c952ef0f68130ccb84a898bc1390b3397db354db8ed15d73a695ca6ddfe4b14d8d0d16be43356978c13",
            "57c8ef4e826f5c662d90380d64f4b1a93dd6b259d6658772c67125c2d6b69d68c5f8143fdab7ea0987bfaee25df5a2c460c60c72fc6a4bb5f7f9af159d45c132"
        },
        {
            "6ed0dd02806fa89e25de060c19d3ac86cabb87d6a0ddd05c333b84f4",
            "21371c287520644985fac6e11a0606cbbcfbdbbf578b3cf633ef5a25",
            "294a03dcad7ae0a71ed75db7f76d71b3bd52f74db48991a94fec3f4e",
            "dde8efbbde8e379060ed089ff3d8278f496b560be07f825a51ceeeed",
            "8a39b752121ee3f7a4978421d6118a8a29b69a49d7a91fb2a012d670",
            "6d1cf656621e9cea1a02aee8ff7ba7c5b5c7fd0d6bff8704bf863f77",
            "77ac3d2247208d480652f7064444934d9f43c2b1690b8f41188452fa",
            "8ca85ae0894e17008344951dbc1fa47980356e4fede7eb86647a020c",
            "a1b0193a8da19da8af8136863219c518e36e3a823583b85168b1dc30",
            "b3494cf1791076d425a57d1f1866754426593846d454e5cd85913b41",
            "e37f191074cb317c16fb6ddab26472a54803026f0d7cdcedcf7caad1",
            "d2c01c819ff3300903324c123bf593fbb37cdd2156eb2e0cd13eaaf7",
            "6e68332448740e16759accba51d911a53ff45ea72b7abaa3e79bcc04",
            "771e85cf0fd6e8bcf72e33e82680100510527515191d6a4efc7f5fa4",
            "7285677f0e699c7433e5a4194f634898dcf9b88e0843b7cf63dbb82c",
            "453db0b59812f1616671bc6ac26772541c73163b4dbe8f0a81fe9a4e",
            "9c8eae6f58e22ac2841015ae7bbb07a6a9269f03024119158a6c14c0",
            "9fb50d57a9d1f6e81ab90531cbd1271fa30c19aec256f842daeb5029",
            "76b4a6567aef60fd6fa431f244410fff5d50bb1381044da84dd73651",
            "602c22955a3d07e1e8cbc05d9e35242ffbdd876c761866d80abf1eb8",
            "978c4ba79f1e35389b728ce7c61de36bedc9086ffae3f4f0b3b7576f",
            "882c4a7ff65c4755ee7b4875ef90464661bf09f544cf5d88dabb078e",
            "6fd7e6c64e4d24a3809fe7180e689cf9d120e8276a47bfad2f2ca958",
            "49c027696567dbcf5a438f125b4c480e92c255f49b7cccfb53652fcc",
            "bfec28e01815e567f0b5bff0540afd743c7546dbec83c4a4b1991ef7",
            "fa9894c45cd3f06e19544c88f3b7200b4e5710a5a867be90f4b18f4c",
            "36b9e0a9daa40f4de2e69b98746197c5e2751525a1a6f783a3fd56cc",
            "3a53bb6c622be6986dbccbc2fe407ca2183b78e762c924cb5d4769cc",
            "596b993b8d0028c416c2d3487347ba1af59e1bddd714f4c3134f924d",
            "76f4e906d143bad6aa5285b9713670b1fee851d7cf19e4f907b922aa",
            "8f8a6ee367cb357f6d1c7ba07e4b96ff98700dcb0660aa8ec9117d98",
            "0d76e9d7c0191530d1672228906ed536533e2a83a8009df693922762"
        },
        {
            "c672b8d1ef56ed28ab87c3622c5114069bdd3ad7b8f9737498d0c01ecef0967a",
            "6238c7caa345ada50b3aa6c344568f9a50b9af1b6ec9c865b1c1020c0d4aa0cb",
            "beffd9e7a3f24a7c527158ac197c4b6259ceb6ae7f86f8f4fb303309c4d6e352",
            "1d5ea9617d509781d017e8b9439d987365c303443e8ada8284972bacc12b129a",
            "880182093631a3d659c24ffaa63427b8149ced5d86f12f94923f5d739a56adf7",
            "bffd8d4b71ffebc9e5a6f3a400bb526b2e5fb77aadcc72ae87df51ddcfacf5bf",
            "71e250e597d0139a88272b24019f58078c8f33c9a6a05144e09531c1015b2ff8",
            "10094730b356bb4b0eec3ce87010f63878d7e264963856b108cc7ecffd60ad41",
            "c09a9cf8a51ec382460c30ae6e394c6ded770cbdeab1d7a4cc3d9bc56125296d",
            "6a376dda927d0045bc6b5c7b34b787a6fc3dd255fe0a3a08202158097b488925",
            "d25b89a9d8e832a2f633eb46f83e00107085cabdba9a37a0d3dbe9b9e3bc1eeb",
            "acb30d5d88c18459a4547481cc7cd0cdf79c24e3c5de4eea253d48d83d0c84d7",
            "754417a2bb2615b8aab1c69989e0a1195da3051884e430b7cefeb0d279be0b70",
            "16914608a1f4a48cc655d8272a1e6bb111c43f9eaa091be71b2566a0f663f323",
            "803be5da76a6dd38a8476e6d58c78c7f3111f950f7c9a18f587e180f79699bfe",
            "df159aeadfd371bd0647aae01a9b37ce0614247e6b37a1841dbc27c9a5615f7f",
            "81fa372bece07562857fda991c20aa2c6955eb981b4b3a8913e3b81f56634ced",
            "4b01a80584cfa37d834383fdf6b5c3d1a6e0c2a69d178acc65548bb0899916e8",
            "39230ef1142d5c56a77cf8aac3a1dc72b7296368949b5f41277622c9b11f9c8b",
            "76cbe7cae1e7af209d9e7e264e7baea0188f36693e84170ff93ca4bb91536884",
            "43997ca9ce733326f113c2af3f1d3028bc900b40b12d733a220a94f71e554b6d",
            "69bd165986fc79a25f1db810e6c0bdc8e9bb4917ecc70dcd202c3de3ef48b641",
            "202a3ddfa4546b6fca7a538d388fb0b89b3dca3e44429dd84846f9422e73569a",
            "814d4711162987691b0368e6e3df472c52e14556d354ca3ffb0d401cec945291",
            "cb0061b5e8c172b220cb5dfb7af0a10da7051c07f9893adf3cd14860cb607e4d",
            "bd2a3c738ce57408897334c48c21153a53b875c8989e077eddf98d1d4a9c193d",
            "2aad3509ac46a16a9883033d469c921f9885b9abcf3a679d3106156c54433382",
            "afeeb8ebaa728540d5f6e62ba96644b991385676362f83c123054cad5f9496c3",
            "d2ea31b3b1a10ef383fd15cf25c081b9ca9e7ded69ddf21ce57794c7a41ae74f",
            "838868780ab97ed76a23198f99a70c7407e3a1aaad757ccb23d9b2b2c3b5fe09",
            "390dd05c4f25e596fbf76802f69c6712af03dea3d9c45da15e677a5c07ea4e3c",
            "6e7d359e20197265794f7f52c6b56b6bdd9c4297f719cd855bcb4c79941dcaaf"
        }
    };
    private static final String[][] FORK_EXPECTED = {
        {
            "8a5e6d9081b08ada24918c6a8697952bc7c7c92f74a3341eb4a31be93dd425c8781f88c2f2fe40d5f81018ba81a54b48",
            "c238887145a70d7e6bb83ed8fedf39d8a8f7db68624add865556c41d66ebefb7d30e3c58843752ab175ee7d4d3af124f",
            "b5f00ddaeec4b5424b65ea3662dc617ef65d2851414f5462382dfeacd6b3d10a570cdbc7a937f2bcf1e902ef920baaed",
            "79a5689a90663337c42958553fc9d074b0e6f47991ef7103646a2b7f78c776d4bd53e0f824b2c0f01a7bfd02c2ef13c1",
            "c972d905c832fe5d74987c611ba61304d98f501f3f77c3ab3eaf4bac98ccd38f70567a9f79cc85aded96379593d44aa4",
            "83336f0291a3cde727efe7db4078e0bcf2f6a0f6d5b2519dd8db29c0797010ae8b024ead90ec2eaa7f116241179b46d1",
            "432bc5cad53ecbb07007b9c05ebf5fe9795276975244f4d4cd7a43ccaeadf31d4cb71905f9bc83b3eb250f1c74665d41",
            "7361b2d0df2017b4c5357842e6e38e940c6b7d132e7822279e38e55efc6c5b507b4bbe8369cd322ce3ddacd66cd1ac4e",
            "7478068d539698084fa675177dc60443069830713862a65ffa9ff5bca03e422b2182cbf575841d12540b2ae520774040",
            "e0674cbb89502bfc5ce8e4b479a4e5a885fbcbfe02776c8656d98f06757bf8e51843acdd82dd6f59059ace4b366c01f1",
            "163d90d57965d838905cc52618e794f0f7e0f92ebe0a3addf120026615e4cf3237b5a567df2f40b34d881afb80958f7a",
            "9b5154d36299cdcdd5a36053d26b6af724c6c9a854411a50b42987b651b3419bdfa677fd79ccbbe703174ea07246d00d",
            "f0902b187bd4c0071cac4e10b7183c594f6df2de9bfdd20e209de5e999c6ed5da132708db5aaf60202102d740c925daf",
            "ddc3295e3e414c5d4d40c5328e9d9222506fa71b3e3eeb1364a659da254de6f0fa8a37491e68256d20c6829051be84be",
            "39092448bb1005d000b3169b6e1d8944417a55dbfafe938448ead7d71b8dd2d0d6f345e0d1b2fa2fa81f725c27f94204",
            "1f5e11a5e3998964bdd1b77a18eafd7b7b976be1d60215a5ca837f0bc0861095e79252228a0362a061e8dc8103f9dd30",
            "89a9f9e3b6b0ed4a50b3da1e4b4eee5fad04da089f53d7674593ff2a1928a952f3fe55fa4ba1fa8fe43a78e82d818d1a",
            "002b5b243d1374dde0a070dd1375ca9367920a268c696d623bee33ba93d70549581f9487e47cc13e2609fe1d61434acd",
            "5d9654e862f0c394adfc77fa48887942e1f12d465b241b27acd93aae0e4ae6af61884d92d3670006c27c1cfab2e854dd",
            "a5161d839bddf9f3c952f7159e0d2af3d9873b461c4f3b32c13f1c706776023e69bab99c11f4d56c8675fe0e70588de5",
            "60afe37d8009c4f54c02a7176fe1595020d9edba8122ebe6fd8698e468e90a1a191639edea25e78a0dcee90d70e83ab6",
            "53c03060aaa06d1a95fdfff46b2c65af1a59254504a59ecfa956381e10ad69e8a2c23136f886c1949bc04fe2d819bf63",
            "cf4c8ef13a5d3b8858759d07dfcd6294f6bbb6ed73d089914e30b26d521bf334c6b6ed7f3e53fd8855e03d796f1127e7",
            "1572571897fbbb1ea00a317ba85139238144cb325656162d91d5c61b6836a562646de2bd95ed608064498861ba388192",
            "ba00c9742932646baebeacb5afec1aaf0b79f1a29ca0e3a09a3a1a8bad9340a86b66464aa9a14d1eb2a012ba2838b600",
            "454c7dad0adfda5413b0fc301cc079e9cdc0f4606be77045c5eb77f697f189559547cd3e93c4fc756601f52aaa00fe09",
            "f57f6fafc310154ab99f49c9b97cb187d2cc724f354e210602ca34613d927572573b6fb624d045aa7865489384c59ae9",
            "6841bb515e90ca494db482c1ed7d0ecd8480481af7745c8344db5668eec81d1f610da0b68bed1df009f1c87679891937",
            "ba06deb97a99f20f783fa9d215eb106c132474a5725cb0da1686b46a6d7ca5b3df3e5083e0083f93d8b29f6a4187db09",
            "01868a61c7b8433dbc269ef93e403a968c15c008b8f477f6289de968aabee801e1202fbaa177bdffb7a9197d7d75d20d",
            "bd815d4dc845f96d4a92e7cb7c5a313804c89a290812777884694778988844ec861732427e3a47675b33eb5023b092e9",
            "0f967732886f77b128c2c0b232c76e5cfd6fcccd2bd0d2bd2b77b666348f0bb1cebec61441e26cc5a439846b74112c86"
        },
        {
            "848b0779ff415f0af4ea14df9dd1d3c29ac41d836c7808896c4eba19c51ac40a439caf5e61ec88c307c7d619195229412eaa73fb2a5ea20d23cc86a9d8f86a0f",
            "2ddcd5fd66f62c38135603defc728493bc9012c9ea6abc8a9823165792136513b2c53722d2a79c0d6ea31467308bbb49eb217d151d79baf4b2085f487d0abfae",
            "2acb5691f36d2ad035cf1a70a3174c2cdcfdaf71afb4d9b6d09edd440f0260e766e18e6caf88bd8af840c514b7b504a87298c8694ac26ce5210902b0dc5ed766",
            "b7ec0c3cba423d31e4c4846329885ea18ff695c7f3dd23a09ec1f2e16a2a0c437f2333e6a92ac14549c9cb9fda4f773d1d2efbc677b38bf97ab2058fde7f861a",
            "03f0fa41fa5eb9a178e66385107900b3fa92f9f7a06bd17dbed7ae01077e18156bc733e78d612ae620580defe7a2f518aecffcbca9225a134eae7018686d8b72",
            "2127217688dea6df27f9eb41e472da1f75564e9a88be503c6f862df9246070e93c05e02e8d806966320d069d8217266aee0bb46bf2c7b74e8e4069761c0d9f30",
            "a8af4cdb6f421a5c1a59e108bd4ad1fc08a838b61c1e742063b0008828f6a6b258a612743a8ff370d3a4b03a53bd325e962c31fc4a102777e78af57ccdaa440a",
            "690d807da3e588ca4e1b99705cf1fcf745a26f9c665d605fc76da8c55424f82600ff4cd4c9a0cecfc2a75749c2a34f2368748b3ec861367595366c8107c03783",
            "3677e0d2e28b467c1efe9d3ba605280bebd7e45c701e93cd3876f7adb7717ef88ce3e094cc8f7c26dd57aff5c87d1f6e8f58480a9a8408a1fc79c949b20da74f",
            "e1021ca425c703342c54cc970009381eb3d2cd2838c5fd8d05d4745c1615d16e7c297161ee8342054dafa3e28a2e89b193c014273b05be9da4195835d6c2dae7",
            "f85930b0a02a0280fb0a94bb27ebb9571cd2c3c9ef03b1c41ad0056c8b373de317e12f78f47d4df5642dbf3e44a41b86d0981b4b57e740932f8f285663d6ccd7",
            "c689387863aa16a0bdefdcd09cf407c553a9627b1dc12e942007ec5855dc015386df6eecd1c71acf673bc035a0ca957ae0d5686166fc7f8a072616126c8bb250",
            "34151d3fd563ece4acebba592ae3e212911aa1c18143d4bbc39ac1210a8e5ae51d33d16d38e045e2221b9ffcf41100c25f7f8e9324550606caf692e4a3d45c2e",
            "61a69777132ce301d445b22bd0b40b0d856befb271716b24b89084b914a8ef4e21afe656bb6f8613416e28ab3347ef3e65476c0ea6fb572f097bbbc3d21793ca",
            "24c7a501c85ab2bb6067e8680432976615d4409ae6dd0d7965212e50e5b5c69c193f2cc8cbb821cfa60a810425421e192dcd6c57a363a32b96252b27f378d8d7",
            "b4f1f523117a800ce8853917eaf2032f5f2cfea2751900552c25855eb8360ca2c9185e7a3ac23b5d8f0b951a97c699e44aa6bfa175f678fba2617df64323a593",
            "7ad81b4e01be141cf03fbc1239ffae5d5a00dc8f6b935be74c6437faecd3bd6111ca89d001f4d5e394077c3988ba38f7637b324f021fb80a786dee516d2568bc",
            "2946ecb88a7241daaa942a8dec5c50bdd0e66e903e23a1c4254146df5aa5273495bf7ca2e6e84f2f72c079558e0eb793b5dd5aefeae9220469c14bb147d6a364",
            "2d2cd20055d782a180f3be2227e0c5e9c0ccc0ea1cf3253753c00da965661eb0281d8200434d3ab077a8106bd0851fd291290129910eddc57f3585c582ac917b",
            "04f8b6b8a9243a3827f055bbadec877c8bae4e62e883b10e3557c6bdab662e7413b022827bef9c00543b2eae3d1db0bbf062b7c7f28e04a3186fdcd0548fcfc3",
            "a975fade4c7f62c6ec7210977dd130e4fe759f2b95f33f1e83e426f6b28909e68953db7fd8659c807f10d073e5deb98036d2bdddda2463884cc9ef8fcf8db221",
            "7d3e25f9819b82145a8cb0d4a463497cf29999a825f854f5e7abdb538025aa64011a0a7b9a16a2e501091e354ea4d15c60a09207383a4ec6fd0cf80908800010",
            "2cadfaa0444b75c07cffb6104417ab34af19431b3f22e79bbf5ff9a7ee186b2c04aec8651880906b4668e9769b699206c372e17ec41ebd27a034bfc3fb592185",
            "3a0d5dcb49823b43c27ed0f599d1cf264213f71b1af513279152b1c2906691e33036718482618fc483ed48eb0eafa75548b5ae860d0fe5243d7c7e74c2bbe6f5",
            "6ce9c4eae476a4f80e2191b4c6e06e60f12a8bdde95349a994e20133a95c25ec338bbde9a019bac5076ab4b7ef9fb3ee59691d31f96344da70aa78280d9e2ef6",
            "791a1f9c8d4936929cac588eccf053833ca319ae7659ddcf7e0ce07e8476f6d0e34d0f448f9738a484d8e4d7e89caa4ada041daa93ba8c6ab9c39e1a74fa5644",
            "e40e8d984d732ef6130bd4939eea4c08b667580a00f87ebb4d201385e0cf8d6fa78eda5a275024e9f0e29c00e07453a0754eacb6055a0b44ce493f95c7c09f14",
            "a639b70e47638aa3a8c41316f54169eb66087e8fab44528e158901f4ed43b0b9394a4882649a0c2199076c820506f2a691ff6d4cd4e4770918f4d49eb5d7035a",
            "87d9782a1dca2de4cc59e567f89773e886172150d8f51c03041d04e7435bd57766d4ebacec12f27ed1fdcfe40c6bb7de2b3d9b151f51e4c4ec129165d2da650f",
            "42e68639f1be18430aa267e76ac62a261c6759ab7e3da54cd28da901337c5c4c617b8be19c04e13a0f46a65e89102f05c599f1941d248e29a0d4a89806407670",
            "d4a143fcb250d9c80e1e618c573dafd407de4d01030e9b1848fd4215f3c5d3a97a8a2d15f38cbfb65fa6da42403f2a1cbbdf3d7c4210dd97cad53f22854524a9",
            "fa4a6d5c84ee88c8dcc3c5758bb003f668fe118bb96036a575e10d9d7a08bba12316193aa83b5e2835213414ac2d5147b977f830b8a44c420dbc2e703f1d099c"
        },
        {
            "874eb61ddb05231cf76d2834411e46ef3a6b36c20af0f0805d1570fb",
            "ebfbb2bee68cb5d96988bc18e3adabac48f8daf471e538668246b554",
            "0aac1d18495db8aba4a1ca27ea5a13423705c6734271135388dda1c2",
            "0dd77c4d4748ecbedcfbfc9e1026492a63cf29836180f291a70b6826",
            "37525fd556d68b16c2ea9f59aa7d70c04602b26c2cddf240eeb5ae48",
            "e459899e7f412170088b7e74717cb1b06c961497d1b5fa90b35678cb",
            "659222e3808e12b827eb81321e17a9736552915d976298453e91e22d",
            "a251e6c68440163285d2dde610bf8d7215b9bd73d20012d3737c1477",
            "8a216631f5341a57ee4b70159caeaac7a9137d4e4ff6f009a2c6d0b9",
            "f05abe0da1706c9a6244e9af2e6c3e7a62f34ae68a24d150274e47da",
            "589846dba7b1958431331249ebf69826006b73e314814b260d8ecdef",
            "198c2767cbffe587ef42dc7f5e8b794b2e31618452bfce2d90499e19",
            "8cdc7b43891d67310b7ee6fd0daa12fe4005b074fc3efa7331a68603",
            "536fa0d06825348fbe4d001966035525e02c42ba26e782e4587d2128",
            "06933827d479e6013e0ab1ffaa7f11f39a6899c71dee2c428f97095a",
            "b7eb8e73040c3cd24ec36e37e8f40592e15c7379eb4918f6f80e9fac",
            "000533a02c1ca0324adfc928ce5fc0dcf0760ed16e6b832caad03550",
            "2f7d402ef7193dd0129d4f5b5eae0fd245fae803897edb3274000e7d",
            "31c60772bf17e924e1c6100a7df0bd52107ad789fd03543cf2c09408",
            "d6149acfe653d85783fb972861b3ec38cdeae29fa8f9572660ef40e1",
            "a5273e444075a3da399ba802c3d7b447a7ca20ddfafd537cd368e069",
            "b8c627431eaa963967b97577ad4e8d3a04e409bd92210efa6cadb75e",
            "44aae2d59c37a24bde1194fa4fd5a6155209032209a9d71eb5a004aa",
            "e3c631d5a62a1a45f746d4464fa0376f087d7fbe2ea3703eb27bf257",
            "919b568f23166ec1763e129ae1554156f471d8238840b5af201f01c4",
            "9294a314c0dd04a6b579459b503f6ca3509d2aa58e07732029bc0f83",
            "81ef16d1de2f459037ea879fae48756ff251c82337a56276a490cbbb",
            "63214c8fae7a1588eb0e42a2c005e402e2b976411f0dd2d4daffe020",
            "fc45ed9e297d5f46355da3566ec633c17690ca315b30b86e763a944d",
            "7c90b10ceecb9ac86d5d59c24ff09b20955ddf5409a62933028d3145",
            "73cd8b0f57f7a13b61db027fd69df8baf4db4d6f7bf0028159c5837a",
            "fe684e93462f402f05ab49fc9b829b7427fadb9a472d5d2992ca980c"
        },
        {
            "1f90f6edff518ca45ac3dfb20aaf317367392275c60ad38a697b49a8a3899ed5",
            "3c38ee2d4da26dd25e8d2b0963281340cea26db9a7784daca90cec041db45dbb",
            "f26a3c3a8cab05318ed6e10d37f72c8e1a2886643279efa3535a73f9e6b1fb9a",
            "31443cb6ff5d60905de414eb9f97f7efb1d24ea4ca9c76653b58cb66c9b346a1",
            "ca7723cef9e24900602d547826c77a444b8bab93c117e46d2d6dd197602661c2",
            "b651e923ac848cff6b6a42b57cbd32e2afc477acd20677b4ae991e5df4e5d006",
            "045c9ff4681218bf2afc38f64d8e806adb4271f57182ed980ce169e609e36b3b",
            "aa8b730bf63222ea0bb312bf1d80f2f936261f9eb77b3422eef173223026bb66",
            "4e1417db3d01b0aed8970726399d1fa07676b3adc8cd717a6068fde195ec3383",
            "e446d3c5e08e4e059506130006e340da35285cc5b6f7ec4f51416396330b866b",
            "6edb02ce3a76a10b3c4c8d142538aa4cef9134e639b63d7faa8f2e9371af38c4",
            "9398e6e709c1c1a57b2549722e71165bb4d82198af7f269dd0d9a2243d44e620",
            "1aa7d1a72390582c1a2acfbeadf00de3f5d35019057930508477bfb4c1f1f4fe",
            "11357fd1d4c6561b7cd77a3ea4ee60c62e45c9a3b8d87720170814d439392ff4",
            "66180961e101f7ed66d769ab45fe43524d8bde7b269c583906ecb1715d7be4c8",
            "f8234a5c4a693bd3d613654b5b9a6da1f7eefd5b13f161b217444edab623201e",
            "3dda9c8ace971cf9cbc44fdba5ae16482dfe8c876b8e1dfe5f44fef6f62ad942",
            "49cbfd6ddc30d035d8dcd28924a5a097053b40a343008463292a2c61e488ccfd",
            "2b2832780b92b97b8ff82ae84514d4e9c97fac8e1b9b9226e6d993c953675be3",
            "5a724b6efac59d25f8e2fbb52c026c64f245497b90489793ec6b3f3a09919ffa",
            "77b490b7c21ab64cb508e687cad759c4560eb2474a208d027b9355f941ad50e5",
            "fa72f5e3c6b18adfd1125b739a4a2286ef892a8bfc68ce56e7bed1687034ebd0",
            "3ca9790a709d5dbf99ffd6fa598f1f60ce8d348a70285d45c6340ee086d09861",
            "aaf31a60fd1e51482056f9aae4bbab21431a2d5288487966913b37537493d9df",
            "3fbd8442cb29eb2fe1b52abb0dd0345c755a38c195b642872763c0cf6ed8aa57",
            "1d1b745a7883e866553e272f75631db55582920f4e13a3ee19c79810a9e17ed0",
            "24b82407d99ed3a69ae8598de5b8bb4424d7af08029cac9695b2b9468e9a1ca1",
            "0a95dada1840093525947179306a5930bedcf4b826e34f0ca5cea0a69f81b996",
            "53b0f0bbe8aa52242f505f66238e7b624e4d2cc31d604de7ecf2ff9bb8c56734",
            "d7d0710032330c922c64921a1d477f7a9865fcee6b1ca8ad4bc3917e17813424",
            "9813604b79e4d21fe205b0decb3201297a414005ae3aba96a0c722f4ca58ba74",
            "0d9768e638c2297e129650dc715464e1e811a48f07866fd03a6d9718d9224111"
        }
    };

    private static byte[] data(int length) {
        byte[] result = new byte[length];
        for (int i = 0; i < length; ++i) result[i] = (byte)(i * 37 + (i >>> 3) + 11);
        return result;
    }
    private static void check(String description, boolean valid) {
        if (!valid) throw new AssertionError(description);
        ++checks;
    }
    private static void digestIs(String description, byte[] actual, String expected) {
        check(description, HexFormat.of().formatHex(actual).equals(expected));
    }
    private static MessageDigest digest(String algorithm) throws Exception {
        // Select the provider whose SHA5 implementation is being ported.
        return MessageDigest.getInstance(algorithm, "SUN");
    }
    private static void algorithm(int index) throws Exception {
        String name = ALGORITHMS[index];
        digestIs(name + ".abc", digest(name).digest("abc".getBytes(StandardCharsets.US_ASCII)), ABC[index]);
        for (int vector = 0; vector < LENGTHS.length; ++vector) {
            int length = LENGTHS[vector];
            String label = name + ".length=" + length;
            byte[] input = data(length);
            String expected = EXPECTED[index][vector];
            MessageDigest hash = digest(name);
            check(label + ".reported_length", hash.getDigestLength() * 2 == expected.length());
            digestIs(label + ".array", hash.digest(input), expected);
            // digest() must reset the object back to the algorithm's IV.
            digestIs(label + ".implicit_reset", hash.digest(input), expected);

            byte[] padded = new byte[length + 67];
            Arrays.fill(padded, (byte)0xa5);
            System.arraycopy(input, 0, padded, 31, length);
            byte[] snapshot = padded.clone();
            hash.update(padded, 31, length);
            digestIs(label + ".offset31_remainder", hash.digest(), expected);
            check(label + ".input_unchanged", Arrays.equals(padded, snapshot));

            int[] sizes = {1, 7, 127, 128, 129, 8191, 8192, 3, 16385};
            int consumed = 0, step = 0;
            while (consumed < length) {
                int count = Math.min(sizes[step++ % sizes.length], length - consumed);
                hash.update(padded, 31 + consumed, 0);
                hash.update(padded, 31 + consumed, count);
                consumed += count;
            }
            digestIs(label + ".chunks", hash.digest(), expected);

            ByteBuffer direct = ByteBuffer.allocateDirect(length + 29);
            direct.position(13);
            direct.put(input);
            direct.limit(13 + length).position(13);
            hash.update(direct);
            check(label + ".direct_consumed", direct.position() == direct.limit());
            digestIs(label + ".direct", hash.digest(), expected);

            ByteBuffer readOnly = ByteBuffer.wrap(padded, 31, length).asReadOnlyBuffer();
            hash.update(readOnly);
            check(label + ".readonly_consumed", !readOnly.hasRemaining());
            digestIs(label + ".readonly", hash.digest(), expected);

            int prefix = length >= 257 ? 193 : length / 2;
            hash.update(input, 0, prefix);
            MessageDigest fork = (MessageDigest)hash.clone();
            hash.update(input, prefix, length - prefix);
            digestIs(label + ".clone_original", hash.digest(), expected);
            byte[] different = length == 0 ? new byte[] {0x42} : input.clone();
            if (length != 0) different[length - 1] ^= 0x5a;
            fork.update(different, prefix, different.length - prefix);
            digestIs(label + ".clone_independent", fork.digest(), FORK_EXPECTED[index][vector]);

            hash.update(data(193));
            hash.reset();
            digestIs(label + ".explicit_reset", hash.digest(input), expected);
            hash.update(input);
            byte[] output = new byte[hash.getDigestLength() + 17];
            Arrays.fill(output, (byte)0x5c);
            int written = hash.digest(output, 7, hash.getDigestLength());
            check(label + ".output_length", written * 2 == expected.length());
            digestIs(label + ".output_offset", Arrays.copyOfRange(output, 7, 7 + written), expected);
            boolean guards = true;
            for (int i = 0; i < 7; ++i) guards &= output[i] == 0x5c;
            for (int i = 7 + written; i < output.length; ++i) guards &= output[i] == 0x5c;
            check(label + ".output_guards", guards);
        }
        System.out.println("sha5.algorithm PASS name=" + name + " checks=" + checks);
    }

    private static void signedJars(String intactName, String alteredName) throws Exception {
        try (JarFile intact = new JarFile(Path.of(intactName).toFile(), true)) {
            JarEntry entry = intact.getJarEntry("payload.bin");
            check("jar.intact_payload_present", entry != null);
            MessageDigest hash = digest("SHA-512");
            long bytes = 0;
            try (InputStream input = intact.getInputStream(entry)) {
                byte[] buffer = new byte[4093];
                int n;
                while ((n = input.read(buffer)) >= 0) {
                    if (n == 0) continue;
                    hash.update(buffer, 0, n);
                    bytes += n;
                }
            }
            check("jar.intact_payload_size", bytes == 32769);
            digestIs("jar.intact_payload_digest", hash.digest(), EXPECTED[1][30]);
            CodeSigner[] signers = entry.getCodeSigners();
            check("jar.intact_authenticated_signer", signers != null && signers.length > 0
                    && !signers[0].getSignerCertPath().getCertificates().isEmpty());
        }
        boolean rejected = false;
        try (JarFile altered = new JarFile(Path.of(alteredName).toFile(), true)) {
            JarEntry entry = altered.getJarEntry("payload.bin");
            check("jar.tampered_payload_present", entry != null);
            try (InputStream input = altered.getInputStream(entry)) {
                byte[] buffer = new byte[4093];
                while (input.read(buffer) != -1) { }
            }
        } catch (SecurityException expected) {
            // A decompression/IO failure does not count as signature rejection.
            rejected = true;
            System.out.println("sha5.jar TAMPER_REJECTED type=" + expected.getClass().getName());
        }
        check("jar.tamper_rejected", rejected);
        System.out.println("sha5.jar SIGNATURE_VALIDATION_PASS");
    }

    public static void main(String[] arguments) throws Exception {
        if (arguments.length != 0 && arguments.length != 2)
            throw new IllegalArgumentException("Usage: HorizonSha5Probe [intact.jar tampered.jar]");
        System.out.println("HORIZON_SHA5_BEGIN oracle=Python-OpenSSL algorithms=4");
        for (int i = 0; i < ALGORITHMS.length; ++i) algorithm(i);
        if (arguments.length == 2) signedJars(arguments[0], arguments[1]);
        System.out.println("HORIZON_SHA5_PASS checks=" + checks + " signed_jars=" + (arguments.length == 2));
    }
}
