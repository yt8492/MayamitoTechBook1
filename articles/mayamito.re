={yt8492} Kotlin/Nativeでもサーバーサイドしたい!

こんにちは、マヤミトです。

Kotlinで使えるサーバーサイドのフレームワークに、Ktorがあります。
Ktorは元々JVMで動かす前提のフレームワークですが、バージョン2系からなんとKotlin/Nativeのサポートが入りました。
今回は、そんなKtorをKotlin/Nativeで実際に動かしてみたいと思います。

== 作るもの

サーバーサイドの開発がある程度できることを手っ取り早く検証するには、とりあえずTodoアプリかなと思います。

というわけで、Todoの作成、取得、更新、削除のエンドポイントがあるサーバーを実装してみましょう。

なお、ある程度実用的な構成にしたいので、Todoの保存はインメモリではなくPostgreSQLを使います。

== 使用するツールのバージョン

今回使用するツールのバージョンは以下の通りです。

 * IntelliJ IDEA 2023.1 (Community Edition)
 * Gradle 8.2
 * OpenJDK 17

== プロジェクトのセットアップ

IntelliJ IDEAで新しくKotlinのプロジェクトを作ります。まずはプロジェクトのbuild.gradle.ktsを見てみましょう。

//listnum[sample1][build.gradle.kts][kotlin]{
plugins {
  kotlin("multiplatform") version "1.8.22"
  kotlin("plugin.serialization") version "1.8.22"
  id("app.cash.sqldelight") version "2.0.0-rc02"
}

group = "com.yt8492"
version = "1.0-SNAPSHOT"

repositories {
  mavenCentral()
}

kotlin {
  val hostOs = System.getProperty("os.name")
  val arch = System.getProperty("os.arch")
  val nativeTarget = when {
    hostOs == "Mac OS X" && arch == "x86_64" -> macosX64("native")
    hostOs == "Mac OS X" && arch == "aarch64" -> macosArm64("native")
    hostOs == "Linux" -> linuxX64("native")
    else -> throw GradleException("Host OS is not supported in Kotlin/Native.")
  }

  nativeTarget.apply {
    binaries {
      executable {
        entryPoint = "main"
      }
    }
  }
  sourceSets {
    val nativeMain by getting {
      dependencies {
        val ktorVersion = "2.3.2"
        implementation("io.ktor:ktor-server-core:$ktorVersion")
        implementation("io.ktor:ktor-server-cio:$ktorVersion")
        implementation("io.ktor:ktor-server-content-negotiation:$ktorVersion")
        implementation("io.ktor:ktor-serialization-kotlinx-json:$ktorVersion")

        implementation("app.cash.sqldelight:native-driver:2.0.0-rc02")
        implementation("app.softwork:postgres-native-sqldelight-driver:0.0.9")

        implementation("app.softwork:kotlinx-uuid-core:0.0.20")

        implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.5.1")
        implementation("org.jetbrains.kotlinx:kotlinx-datetime:0.4.0")
      }
    }
    val nativeTest by getting
  }
}

sqldelight {
  databases {
    register("NativePostgres") {
      dialect("app.softwork:postgres-native-sqldelight-dialect:0.0.9")
      packageName.set("com.yt8492.ktornative")
    }
    linkSqlite.set(false)
  }
}
//}

今回はKotlin 1.8.22, Ktor 2.3.2を使います。

DBとの通信はSQLDelightを使います。
SQLDelightはもともとKotlin/NativeではSQLiteにしか対応していませんでしたが、最近になってpostgres-native-sqldelight-driverが追加されたことにより、Kotlin/NativeでもPostgreSQLが使えるようになりました。

なお、KtorのKotlin/Native対応は、執筆時点ではWindowsに対応していません。そのため、システムプロパティからOSとアーキテクチャを見て、Windowsの場合はビルドが失敗するようにしています。

次に、gradle.propertiesを見てみましょう。

//listnum[sample2][gradle.properties][properties]{
kotlin.code.style=official
kotlin.native.cacheKind.macosX64=none
kotlin.native.cacheKind.macosArm64=none
kotlin.native.cacheKind.linuxX64=none
//}

cacheKindの設定を追加していますが、これをしないとビルドが通らないので注意です。

== DBのセットアップ

今回はPostgreSQLを使います。簡単にローカルでdocker-composeで起動できるように、compose.ymlを書きましょう。

//listnum[sample3][compose.yml][yml]{
services:
  db:
    image: postgres:15.3-alpine3.18
    container_name: todo-db
    restart: always
    ports:
      - "5432:5432"
    volumes:
      - db-store:/var/lib/postgresql/data
      - ./postgres/init:/docker-entrypoint-initdb.d
    environment:
      POSTGRES_USER: postgres
      POSTGRES_DB: postgres
      POSTGRES_PASSWORD: password
volumes:
  db-store:
//}

volumesに起動時に実行するSQLの設定をしています。postgres/init ディレクトリにCREATE TABLEするSQLを置きましょう。

//listnum[sample4][init.sql][sql]{
CREATE TABLE todos (
  id TEXT PRIMARY KEY NOT NULL UNIQUE,
  title TEXT NOT NULL,
  content TEXT NOT NULL,
  create_at TEXT NOT NULL,
  update_at TEXT NOT NULL
);
//}

これで、docker compose upした際にtodosテーブルが作成されます。

idをINTEGER AUTOINCREMENTにしていないのは、SQLDelightのPostgreSQLドライバーが対応していなかったからです。今回はアプリケーション側でidを生成する方針でやります。

== SQLDelight用にsqファイルを書く

今回はSQLDelightを使ってPostgreSQLと通信します。
SQLDelightは、.sqファイルにテーブルとその操作をSQLで記述し、それをもとにKotlinコードを自動生成します。

.sqファイルは、src/commonMain/sqldelight 以下の階層に配置します。build.gradle.ktsでpackageName.set()で指定したpackage名と階層が一致しないとエラーになるので注意です。

今回の例では、 "com.yt8492.ktornative" を指定しているので、 src/commonMain/sqldelight/com/yt8492/ktornative 以下に.sqファイルを配置します。

== Todoサーバーの実装

準備が長くなりましたが、ここから本格的にTodoサーバーの実装に入ります。

といっても、Todoはタイトルと本文があるだけのシンプルなもので、作成、取得、更新、削除ができるだけの単純なものです。

=== modelの定義

Todo型を定義します。

//listnum[sample5][Todo.kt][kotlin]{
@Serializable
data class Todo(
  val id: String,
  val title: String,
  val content: String,
  @SerialName("create_at")
  val createAt: Instant,
  @SerialName("update_at")
  val updateAt: Instant,
)
//}

今回はKtorでクライアントにJSONで返すようにします。serialize/deserializeにはkotlinx-serializationを使います。

createAtとupdateAtにInstantを指定しています。これはkotlinx-datetimeのものを使っています。

Todoの作成と更新で受け取るJSONの型も同じく定義します。

//listnum[sample6][Request.kt][kotlin]{
@Serializable
data class CreateTodoRequest(
    val title: String,
    val content: String,
)

@Serializable
data class UpdateTodoRequest(
    val title: String?,
    val content: String?,
)
//}

=== repositoryの実装

次に、DBの操作をまとめたRepositoryクラスを実装します。

//listnum[sample7][TodoRepository.kt][kotlin]{
class TodoRepository(
  private val todoQueries: TodoQueries,
) {
  fun findAll(): List<Todo> {
    return todoQueries.selectAll()
      .executeAsList()
      .map {
        toModel(it)
      }
  }

  fun find(id: String): Todo? {
    return todoQueries.select(id)
      .executeAsOneOrNull()
      ?.let {
        toModel(it)
      }
  }

  fun create(title: String, content: String): Todo {
    val now = Clock.System.now()
    val id = UUID.generateUUID()
    todoQueries.insert(
      id = id.toString(false),
      title = title,
      content = content,
      create_at = now.toString(),
      update_at = now.toString(),
    )
    return Todo(
      id = id.toString(false),
      title = title,
      content = content,
      createAt = now,
      updateAt = now,
    )
  }

  fun update(todo: Todo): Todo {
    val now = Clock.System.now()
    todoQueries.update(
      id = todo.id,
      title = todo.title,
      content = todo.content,
      update_at = now.toString(),
    )
    return todo.copy(
      updateAt = now,
    )
  }

  fun delete(id: String) {
    todoQueries.delete(id)
  }

  private fun toModel(todos: Todos): Todo {
    return Todo(
      id = todos.id,
      title = todos.title,
      content = todos.content,
      createAt = Instant.parse(todos.create_at),
      updateAt = Instant.parse(todos.update_at),
    )
  }
}
//}

TodoQueriesは、SQLDelightの.sqファイルから自動生成された型です。このオブジェクトを介して、.sqファイルに定義した操作を実行することができます。

.sqファイルに書いたCREATE TABLE文をもとに対応する型が自動生成されますが、そのままでは不便なのでこちらで定義したTodo型に変換して使っています。

=== ルーティングの定義



KtorのApplicationクラスの拡張関数として定義します。引数に先程実装したTodoRepositoryを受け取るようにします。

今回は単純なTodoアプリなので、ファイルは分けず全部のエンドポイントをここで定義しています。

//listnum[sample8][Application.kt][kotlin]{
fun Application.module(
    todoRepository: TodoRepository,
) {
    routing {
        post("/todos") {
            val request = call.receive<CreateTodoRequest>()
            val todo = todoRepository.create(
                title = request.title,
                content = request.content,
            )
            call.respond(HttpStatusCode.Created, todo)
        }
        get("/todos") {
            val todos = todoRepository.findAll()
            call.respond(HttpStatusCode.OK, todos)
        }
        get("/todos/{id}") {
            val id = call.parameters["id"]
            if (id == null) {
                call.respond(HttpStatusCode.BadRequest)
                return@get
            }
            val todo = todoRepository.find(id)
            if (todo == null) {
                call.respond(HttpStatusCode.NotFound)
                return@get
            }
            call.respond(HttpStatusCode.OK, todo)
        }
        patch("/todos/{id}") {
            val id = call.parameters["id"]
            if (id == null) {
                call.respond(HttpStatusCode.BadRequest)
                return@patch
            }
            val request = call.receive<UpdateTodoRequest>()
            val todo = todoRepository.find(id)
            if (todo == null) {
                call.respond(HttpStatusCode.NotFound)
                return@patch
            }
            val updated = todoRepository.update(
                todo.copy(
                    title = request.title ?: todo.title,
                    content = request.content ?: todo.content,
                )
            )
            call.respond(HttpStatusCode.OK, updated)
        }
        delete("/todos/{id}") {
            val id = call.parameters["id"]
            if (id == null) {
                call.respond(HttpStatusCode.BadRequest)
                return@delete
            }
            val todo = todoRepository.find(id)
            if (todo == null) {
                call.respond(HttpStatusCode.NotFound)
                return@delete
            }
            todoRepository.delete(id)
            call.respond(HttpStatusCode.NoContent)
        }
    }
}
//}

JVMのKtorと同じように普通にルーティングの実装ができていますね。

=== main関数の実装

エントリーポイントとなる、main関数を実装します。

//listnum[sample9][Main.kt][kotlin]{
fun main() {
  val coroutineContext = Dispatchers.Default + Job()
  val coroutineScope = CoroutineScope(coroutineContext)
  val dbHost = getenv("POSTGRES_HOST")?.toKString()
    ?: throw IllegalArgumentException("POSTGRES_HOST must not be null")
  val dbPort = getenv("POSTGRES_PORT")?.toKString()?.toInt()
    ?: throw IllegalArgumentException("POSTGRES_PORT must not be null")
  val dbUser = getenv("POSTGRES_USER")?.toKString()
    ?: throw IllegalArgumentException("POSTGRES_USER must not be null")
  val dbName = getenv("POSTGRES_DB")?.toKString()
    ?: throw IllegalArgumentException("POSTGRES_DB must not be null")
  val dbPassword = getenv("POSTGRES_PASSWORD")?.toKString()
    ?: throw IllegalArgumentException("POSTGRES_PASSWORD must not be null")
  val driver = PostgresNativeDriver(
    host = dbHost,
    port = dbPort,
    user = dbUser,
    database = dbName,
    password = dbPassword,
    options = null,
    listenerSupport = ListenerSupport.Remote(coroutineScope),
  )
  val database = NativePostgres(driver)
  val todoRepository = TodoRepository(database.todoQueries)
  embeddedServer(
    factory = CIO,
    port = 8080,
    host = "0.0.0.0",
    module = {
      install(ContentNegotiation) {
        json()
      }
      module(todoRepository)
    },
  ).start(wait = true)
}
//}

今回はやりませんが、最終的にdocker-composeで動かすことも考え、DBの接続先の情報は環境変数で受け取るようにしています。

embeddedServer関数に必要な値を渡して設定し、startメソッドを呼び出して起動しています。

これで実装は完了です。

== ビルドして実行する

実装が終わったら、ネイティブで実行可能なバイナリをビルドしましょう。

nativeMainBinariesというGradleタスクを実行して、実行可能なバイナリを生成することができます。

//listnum[sample10][build][bash]{
./gradlew nativeMainBinaries
//}

ビルドすると、 build/bin/native/releaseExecutable 以下にバイナリが生成されるので、それを実行するとサーバーがローカルで立ち上がります。

今回はDBの接続情報を環境変数で渡すようにしているので、環境変数の設定、ビルド、実行の一連の流れをshellのファイルにまとめておくと便利です。

//listnum[sample11][run.sh][shell]{
export POSTGRES_HOST=localhost
export POSTGRES_PORT=5432
export POSTGRES_USER=postgres
export POSTGRES_DB=postgres
export POSTGRES_PASSWORD=password
export KTOR_LOG_LEVEL=TRACE
./gradlew nativeMainBinaries
./build/bin/native/releaseExecutable/ktor-native.kexe
//}

KTOR_LOG_LEVELという環境変数を使うことで、リクエストを受け取った際に出力するログのレベルを制御することができます。指定可能な値は次の通りです。

 * TRACE
 * DEBUG
 * INFO
 * WARN
 * ERROR

早速実行してサーバーを起動してみましょう。DBのdockerを立ち上げていない場合はdocker-composeで立ち上げるのを忘れないようにしてください。

//listnum[sample12][run][shell]{
docker compose up -d
./run.sh

[INFO] (ktor.application): Application started in 0.0 seconds.
[INFO] (ktor.application): Responding at http://0.0.0.0:8080
//}

先程用意した実行用のシェルスクリプトを実行すると、ビルドが走った後サーバーが立ち上がると思います。試しにcurlでTODOの作成と取得のリクエストを送ってみましょう。

//listnum[sample13][request][shell]{
curl -X POST \
-H 'Content-Type: application/json' \
-d '{"title": "hoge", "content": "fuga"}' \
'localhost:8080/todos'

{
  "id":"ffff93ff-ffff-4400-bfec-17ffffffb900",
  "title":"hoge",
  "content":"fuga",
  "create_at":"2023-07-25T15:16:38.571Z",
  "update_at":"2023-07-25T15:16:38.571Z"
}


curl 'localhost:8080/todos'

[
  {
    "id":"ffff93ff-ffff-4400-bfec-17ffffffb900",
    "title":"hoge",
    "content":"fuga",
    "create_at":"2023-07-25T15:16:38.571Z",
    "update_at":"2023-07-25T15:16:38.571Z"
  }
]
//}

このように、ちゃんとレスポンスが返ってくると思います。サーバー側のログも見てみましょう。

//listnum[sample14][log][shell]{
Matched routes:
  "" -> "todos" -> "(method:POST)"
Route resolve result:
  SUCCESS @ /todos/(method:POST)
[TRACE] (io.ktor.server.engine.DefaultTransform): No Default Transformations
  found for class io.ktor.utils.io.ByteChannelNative and expected type
TypeInfo(type=class com.yt8492.ktornative.model.CreateTodoRequest,
reifiedType=com.yt8492.ktornative.model.CreateTodoRequest,
kotlinType=com.yt8492.ktornative.model.CreateTodoRequest) for call /todos
[TRACE] (io.ktor.routing.Routing): Trace for [todos]
/, segment:0 -> SUCCESS @ /
  /todos, segment:1 -> SUCCESS @ /todos
    /todos/(method:POST), segment:1 -> FAILURE "Selector didn't match"
@ /todos/(method:POST)
    /todos/(method:GET), segment:1 -> SUCCESS @ /todos/(method:GET)
    /todos/{id}, segment:1 -> FAILURE "Selector didn't match" @ /todos/{id}
Matched routes:
  "" -> "todos" -> "(method:GET)"
Route resolve result:
  SUCCESS @ /todos/(method:GET)
//}

このように、リクエストのトレースが出力されているのがわかると思います。

これで、KtorとKotlin/Nativeを用いたTodoアプリの実装は完了です。

== 今回のサンプル実装のリンク

今回のサンプル実装は、マヤミトのGitHubで公開しています。今後アップデートするかもしれないので、興味があれば見てみてください。

//image[mayamito-1][repository]

https://github.com/yt8492/ktor-native-server-sample

== おわりに

Kotlin/Nativeはここ最近になって使えるライブラリ類が充実してきています。特に、今回紹介したKtorとSQLDelight+PostgreSQLドライバーの登場は、サーバーサイドKotlin/Nativeの実用化に向けた大きな一歩となるでしょう。

まだまだやる人間の少ない分野ではありますが、Kotlin/Nativeも徐々に盛り上がりを見せているので、この機会に皆さんもぜひ試してみてください。